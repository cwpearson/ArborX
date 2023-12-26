/****************************************************************************
 * Copyright (c) 2023 by the ArborX authors                                 *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the ArborX library. ArborX is                       *
 * distributed under a BSD 3-clause license. For the licensing terms see    *
 * the LICENSE file in the top-level directory.                             *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#ifndef ARBORX_INTERP_MOVING_LEAST_SQUARES_HPP
#define ARBORX_INTERP_MOVING_LEAST_SQUARES_HPP

#include <ArborX_AccessTraits.hpp>
#include <ArborX_DetailsLegacy.hpp>
#include <ArborX_GeometryTraits.hpp>
#include <ArborX_HyperBox.hpp>
#include <ArborX_IndexableGetter.hpp>
#include <ArborX_InterpDetailsCompactRadialBasisFunction.hpp>
#include <ArborX_InterpDetailsMovingLeastSquaresCoefficients.hpp>
#include <ArborX_InterpDetailsPolynomialBasis.hpp>
#include <ArborX_LinearBVH.hpp>

#include <Kokkos_Core.hpp>
#include <Kokkos_Profiling_ScopedRegion.hpp>

#include <optional>

namespace ArborX::Interpolation::Details
{

// This is done to avoid a clash with another predicates access trait
template <typename Points>
struct MLSTargetPointsPredicateWrapper
{
  Points target_points;
  int num_neighbors;
};

} // namespace ArborX::Interpolation::Details

namespace ArborX
{

template <typename Points>
struct AccessTraits<
    Interpolation::Details::MLSTargetPointsPredicateWrapper<Points>,
    PredicatesTag>
{
  KOKKOS_INLINE_FUNCTION static auto size(
      Interpolation::Details::MLSTargetPointsPredicateWrapper<Points> const &tp)
  {
    return AccessTraits<Points, PrimitivesTag>::size(tp.target_points);
  }

  KOKKOS_INLINE_FUNCTION static auto
  get(Interpolation::Details::MLSTargetPointsPredicateWrapper<Points> const &tp,
      int const i)
  {
    return nearest(
        AccessTraits<Points, PrimitivesTag>::get(tp.target_points, i),
        tp.num_neighbors);
  }

  using memory_space =
      typename AccessTraits<Points, PrimitivesTag>::memory_space;
};

} // namespace ArborX

namespace ArborX::Interpolation
{

template <typename MemorySpace, typename FloatingCalculationType = double>
class MovingLeastSquares
{
public:
  template <typename ExecutionSpace, typename SourcePoints,
            typename TargetPoints, typename CRBF = CRBF::Wendland<0>,
            typename PolynomialDegree = PolynomialDegree<2>>
  MovingLeastSquares(ExecutionSpace const &space,
                     SourcePoints const &source_points,
                     TargetPoints const &target_points, CRBF = {},
                     PolynomialDegree = {},
                     std::optional<int> num_neighbors = std::nullopt)
  {
    namespace KokkosExt = ArborX::Details::KokkosExt;

    auto guard = Kokkos::Profiling::ScopedRegion("ArborX::MovingLeastSquares");

    static_assert(
        KokkosExt::is_accessible_from<MemorySpace, ExecutionSpace>::value,
        "Memory space must be accessible from the execution space");

    // SourcePoints is an access trait of points
    ArborX::Details::check_valid_access_traits(PrimitivesTag{}, source_points);
    using src_acc = AccessTraits<SourcePoints, PrimitivesTag>;
    static_assert(KokkosExt::is_accessible_from<typename src_acc::memory_space,
                                                ExecutionSpace>::value,
                  "Source points must be accessible from the execution space");
    using src_point =
        typename ArborX::Details::AccessTraitsHelper<src_acc>::type;
    GeometryTraits::check_valid_geometry_traits(src_point{});
    static_assert(GeometryTraits::is_point<src_point>::value,
                  "Source points elements must be points");
    static constexpr int dimension = GeometryTraits::dimension_v<src_point>;

    // TargetPoints is an access trait of points
    ArborX::Details::check_valid_access_traits(PrimitivesTag{}, target_points);
    using tgt_acc = AccessTraits<TargetPoints, PrimitivesTag>;
    static_assert(KokkosExt::is_accessible_from<typename tgt_acc::memory_space,
                                                ExecutionSpace>::value,
                  "Target points must be accessible from the execution space");
    using tgt_point =
        typename ArborX::Details::AccessTraitsHelper<tgt_acc>::type;
    GeometryTraits::check_valid_geometry_traits(tgt_point{});
    static_assert(GeometryTraits::is_point<tgt_point>::value,
                  "Target points elements must be points");
    static_assert(dimension == GeometryTraits::dimension_v<tgt_point>,
                  "Target and source points must have the same dimension");

    int num_neighbors_val =
        num_neighbors ? *num_neighbors
                      : Details::polynomialBasisSize<dimension,
                                                     PolynomialDegree::value>();

    int const num_targets = tgt_acc::size(target_points);
    _source_size = src_acc::size(source_points);
    // There must be enough source points
    KOKKOS_ASSERT(0 < num_neighbors_val && num_neighbors_val <= _source_size);

    // Organize the source points as a tree
    BoundingVolumeHierarchy<MemorySpace, ArborX::PairValueIndex<src_point>>
        source_tree(space, ArborX::Experimental::attach_indices(source_points));

    // Create the predicates
    Details::MLSTargetPointsPredicateWrapper<TargetPoints> predicates{
        target_points, num_neighbors_val};

    // Query the source
    Kokkos::View<int *, MemorySpace> indices(
        "ArborX::MovingLeastSquares::indices", 0);
    Kokkos::View<int *, MemorySpace> offsets(
        "ArborX::MovingLeastSquares::offsets", 0);
    source_tree.query(space, predicates,
                      ArborX::Details::LegacyDefaultCallback{}, indices,
                      offsets);

    // Fill in the value indices object so values can be transferred from a 1D
    // source data to a properly distributed 2D array for each target.
    auto const source_view = fillValuesIndicesAndGetSourceView(
        space, indices, offsets, num_targets, num_neighbors_val, source_points);

    // Compute the moving least squares coefficients
    _coeffs = Details::movingLeastSquaresCoefficients<
        CRBF, PolynomialDegree, FloatingCalculationType, MemorySpace>(
        space, source_view, target_points);
  }

  template <typename ExecutionSpace, typename SourcePoints>
  Kokkos::View<typename ArborX::Details::AccessTraitsHelper<
                   AccessTraits<SourcePoints, PrimitivesTag>>::type **,
               MemorySpace>
  fillValuesIndicesAndGetSourceView(
      ExecutionSpace const &space,
      Kokkos::View<int *, MemorySpace> const &indices,
      Kokkos::View<int *, MemorySpace> const &offsets, int const num_targets,
      int const num_neighbors, SourcePoints const &source_points)
  {
    auto guard = Kokkos::Profiling::ScopedRegion(
        "ArborX::MovingLeastSquares::fillValuesIndicesAndGetSourceView");

    using src_acc = AccessTraits<SourcePoints, PrimitivesTag>;
    using src_point =
        typename ArborX::Details::AccessTraitsHelper<src_acc>::type;

    _values_indices = Kokkos::View<int **, MemorySpace>(
        Kokkos::view_alloc(Kokkos::WithoutInitializing,
                           "ArborX::MovingLeastSquares::values_indices"),
        num_targets, num_neighbors);
    Kokkos::View<src_point **, MemorySpace> source_view(
        Kokkos::view_alloc(Kokkos::WithoutInitializing,
                           "ArborX::MovingLeastSquares::source_view"),
        num_targets, num_neighbors);
    Kokkos::parallel_for(
        "ArborX::MovingLeastSquares::values_indices_and_source_view_fill",
        Kokkos::MDRangePolicy<ExecutionSpace, Kokkos::Rank<2>>(
            space, {0, 0}, {num_targets, num_neighbors}),
        KOKKOS_CLASS_LAMBDA(int const i, int const j) {
          auto index = indices(offsets(i) + j);
          _values_indices(i, j) = index;
          source_view(i, j) = src_acc::get(source_points, index);
        });

    return source_view;
  }

  template <typename ExecutionSpace, typename SourceValues,
            typename ApproxValues>
  void interpolate(ExecutionSpace const &space,
                   SourceValues const &source_values,
                   ApproxValues &approx_values) const
  {
    auto guard = Kokkos::Profiling::ScopedRegion(
        "ArborX::MovingLeastSquares::interpolate");

    namespace KokkosExt = ArborX::Details::KokkosExt;

    static_assert(
        KokkosExt::is_accessible_from<MemorySpace, ExecutionSpace>::value,
        "Memory space must be accessible from the execution space");

    // SourceValues is a 1D view of all source values
    static_assert(Kokkos::is_view_v<SourceValues> && SourceValues::rank == 1,
                  "Source values must be a 1D view of values");
    static_assert(
        KokkosExt::is_accessible_from<typename SourceValues::memory_space,
                                      ExecutionSpace>::value,
        "Source values must be accessible from the execution space");

    // ApproxValues is a 1D view for approximated values
    static_assert(Kokkos::is_view_v<ApproxValues> && ApproxValues::rank == 1,
                  "Approx values must be a 1D view");
    static_assert(
        KokkosExt::is_accessible_from<typename ApproxValues::memory_space,
                                      ExecutionSpace>::value,
        "Approx values must be accessible from the execution space");
    static_assert(!std::is_const_v<typename ApproxValues::value_type>,
                  "Approx values must be writable");

    // Source values must be a valuation on the points so must be as big as the
    // original input
    KOKKOS_ASSERT(_source_size == source_values.extent_int(0));

    using value_t = typename ApproxValues::non_const_value_type;

    int const num_targets = _values_indices.extent(0);
    int const num_neighbors = _values_indices.extent(1);

    KokkosExt::reallocWithoutInitializing(space, approx_values, num_targets);

    Kokkos::parallel_for(
        "ArborX::MovingLeastSquares::target_interpolation",
        Kokkos::RangePolicy<ExecutionSpace>(space, 0, num_targets),
        KOKKOS_CLASS_LAMBDA(int const i) {
          value_t tmp = 0;
          for (int j = 0; j < num_neighbors; j++)
            tmp += _coeffs(i, j) * source_values(_values_indices(i, j));
          approx_values(i) = tmp;
        });
  }

private:
  Kokkos::View<FloatingCalculationType **, MemorySpace> _coeffs;
  Kokkos::View<int **, MemorySpace> _values_indices;
  int _source_size;
};

} // namespace ArborX::Interpolation

#endif