#ifndef OPENMC_RANDOM_RAY_FLAT_SOURCE_DOMAIN_H
#define OPENMC_RANDOM_RAY_FLAT_SOURCE_DOMAIN_H

#include "openmc/openmp_interface.h"
#include "openmc/position.h"
#include "openmc/source.h"

namespace openmc {

//----------------------------------------------------------------------------
// Helper Functions

// The hash_combine function is the standard hash combine function from boost
// that is typically used for combining multiple hash values into a single hash
// as is needed for larger objects being stored in a hash map. The function is
// taken from:
// https://www.boost.org/doc/libs/1_55_0/doc/html/hash/reference.html#boost.hash_combine
// which carries the following license:
//
// Boost Software License - Version 1.0 - August 17th, 2003
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
// The copyright notices in the Software and this entire statement, including
// the above license grant, this restriction and the following disclaimer,
// must be included in all copies of the Software, in whole or in part, and
// all derivative works of the Software, unless such copies or derivative
// works are solely in the form of machine-executable object code generated by
// a source language processor.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
// SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
// FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
inline void hash_combine(size_t& seed, const size_t v)
{
  seed ^= (v + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}

//----------------------------------------------------------------------------
// Helper Structs

// A mapping object that is used to map between a specific random ray
// source region and an OpenMC native tally bin that it should score to
// every iteration.
struct TallyTask {
  int tally_idx;
  int filter_idx;
  int score_idx;
  int score_type;
  TallyTask(int tally_idx, int filter_idx, int score_idx, int score_type)
    : tally_idx(tally_idx), filter_idx(filter_idx), score_idx(score_idx),
      score_type(score_type)
  {}
  TallyTask() = default;

  // Comparison and Hash operators are defined to allow usage of the
  // TallyTask struct as a key in an unordered_set
  bool operator==(const TallyTask& other) const
  {
    return tally_idx == other.tally_idx && filter_idx == other.filter_idx &&
           score_idx == other.score_idx && score_type == other.score_type;
  }

  struct HashFunctor {
    size_t operator()(const TallyTask& task) const
    {
      size_t seed = 0;
      hash_combine(seed, task.tally_idx);
      hash_combine(seed, task.filter_idx);
      hash_combine(seed, task.score_idx);
      hash_combine(seed, task.score_type);
      return seed;
    }
  };
};

/*
 * The FlatSourceDomain class encompasses data and methods for storing
 * scalar flux and source region for all flat source regions in a
 * random ray simulation domain.
 */

class FlatSourceDomain {
public:
  //----------------------------------------------------------------------------
  // Constructors
  FlatSourceDomain();

  //----------------------------------------------------------------------------
  // Methods
  void update_neutron_source(double k_eff);
  double compute_k_eff(double k_eff_old) const;
  void normalize_scalar_flux_and_volumes(
    double total_active_distance_per_iteration);
  int64_t add_source_to_scalar_flux();
  void batch_reset();
  void convert_source_regions_to_tallies();
  void reset_tally_volumes();
  void random_ray_tally();
  void accumulate_iteration_flux();
  void output_to_vtk() const;
  void all_reduce_replicated_source_regions();
  void convert_external_sources();
  void count_external_source_regions();
  double compute_fixed_source_normalization_factor() const;

  //----------------------------------------------------------------------------
  // Static Data members
  static bool volume_normalized_flux_tallies_;

  //----------------------------------------------------------------------------
  // Public Data members

  bool mapped_all_tallies_ {false}; // If all source regions have been visited

  int64_t n_source_regions_ {0}; // Total number of source regions in the model
  int64_t n_external_source_regions_ {0}; // Total number of source regions with
                                          // non-zero external source terms

  // 1D array representing source region starting offset for each OpenMC Cell
  // in model::cells
  vector<int64_t> source_region_offsets_;

  // 1D arrays representing values for all source regions
  vector<OpenMPMutex> lock_;
  vector<int> was_hit_;
  vector<double> volume_;
  vector<int> position_recorded_;
  vector<Position> position_;

  // 2D arrays stored in 1D representing values for all source regions x energy
  // groups
  vector<float> scalar_flux_old_;
  vector<float> scalar_flux_new_;
  vector<float> source_;
  vector<float> external_source_;

private:
  //----------------------------------------------------------------------------
  // Methods
  void apply_external_source_to_source_region(
    Discrete* discrete, double strength_factor, int64_t source_region);
  void apply_external_source_to_cell_instances(int32_t i_cell,
    Discrete* discrete, double strength_factor, int target_material_id,
    const vector<int32_t>& instances);
  void apply_external_source_to_cell_and_children(int32_t i_cell,
    Discrete* discrete, double strength_factor, int32_t target_material_id);

  //----------------------------------------------------------------------------
  // Private data members
  int negroups_;                  // Number of energy groups in simulation
  int64_t n_source_elements_ {0}; // Total number of source regions in the model
                                  // times the number of energy groups

  double
    simulation_volume_; // Total physical volume of the simulation domain, as
                        // defined by the 3D box of the random ray source

  // 2D array representing values for all source elements x tally
  // tasks
  vector<vector<TallyTask>> tally_task_;

  // 1D array representing values for all source regions, with each region
  // containing a set of volume tally tasks. This more complicated data
  // structure is convenient for ensuring that volumes are only tallied once per
  // source region, regardless of how many energy groups are used for tallying.
  vector<std::unordered_set<TallyTask, TallyTask::HashFunctor>> volume_task_;

  // 1D arrays representing values for all source regions
  vector<int> material_;
  vector<double> volume_t_;

  // 2D arrays stored in 1D representing values for all source regions x energy
  // groups
  vector<float> scalar_flux_final_;

  // Volumes for each tally and bin/score combination. This intermediate data
  // structure is used when tallying quantities that must be normalized by
  // volume (i.e., flux). The vector is index by tally index, while the inner 2D
  // xtensor is indexed by bin index and score index in a similar manner to the
  // results tensor in the Tally class, though without the third dimension, as
  // SUM and SUM_SQ do not need to be tracked.
  vector<xt::xtensor<double, 2>> tally_volumes_;

}; // class FlatSourceDomain

//============================================================================
//! Non-member functions
//============================================================================

// Returns the inputted value in big endian byte ordering. If the system is
// little endian, the byte ordering is flipped. If the system is big endian,
// the inputted value is returned as is. This function is necessary as
// .vtk binary files use big endian byte ordering.
template<typename T>
T convert_to_big_endian(T in)
{
  // 4 byte integer
  uint32_t test = 1;

  // 1 byte pointer to first byte of test integer
  uint8_t* ptr = reinterpret_cast<uint8_t*>(&test);

  // If the first byte of test is 0, then the system is big endian. In this
  // case, we don't have to do anything as .vtk files are big endian
  if (*ptr == 0)
    return in;

  // Otherwise, the system is in little endian, so we need to flip the
  // endianness
  uint8_t* orig = reinterpret_cast<uint8_t*>(&in);
  uint8_t swapper[sizeof(T)];
  for (int i = 0; i < sizeof(T); i++) {
    swapper[i] = orig[sizeof(T) - i - 1];
  }
  T out = *reinterpret_cast<T*>(&swapper);
  return out;
}

template<typename T>
void parallel_fill(vector<T>& arr, T value)
{
#pragma omp parallel for schedule(static)
  for (int i = 0; i < arr.size(); i++) {
    arr[i] = value;
  }
}

} // namespace openmc

#endif // OPENMC_RANDOM_RAY_FLAT_SOURCE_DOMAIN_H
