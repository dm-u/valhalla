#ifndef VALHALLA_THOR_COSTMATRIX_H_
#define VALHALLA_THOR_COSTMATRIX_H_

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include <valhalla/baldr/double_bucket_queue.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/graphreader.h>
#include <valhalla/proto/common.pb.h>
#include <valhalla/sif/dynamiccost.h>
#include <valhalla/sif/edgelabel.h>
#include <valhalla/thor/edgestatus.h>
#include <valhalla/thor/matrix_common.h>
#include <valhalla/thor/pathinfo.h>

namespace valhalla {
namespace thor {

// These cost thresholds are in addition to the distance thresholds. If either forward or reverse
// costs exceed the threshold the search is terminated.
constexpr float kCostThresholdAutoDivisor =
    56.0f; // 400 km distance threshold will result in a cost threshold of ~7200 (2 hours)
constexpr float kCostThresholdBicycleDivisor =
    56.0f; // 200 km distance threshold will result in a cost threshold of ~3600 (1 hour)
constexpr float kCostThresholdPedestrianDivisor =
    28.0f; // 200 km distance threshold will result in a cost threshold of ~7200 (2 hours)

/**
 * Status of a location. Tracks remaining locations to be found
 * and a threshold or iterations. When threshold goes to 0 expansion
 * stops for this location.
 */
struct LocationStatus {
  int threshold;
  std::set<uint32_t> remaining_locations;

  LocationStatus(const int t) : threshold(t) {
  }
};

/**
 * Best connection. Information about the best connection found between
 * a source and target pair.
 */
struct BestCandidate {
  bool found;
  baldr::GraphId edgeid;
  baldr::GraphId opp_edgeid;
  sif::Cost cost;
  uint32_t distance;
  uint32_t threshold;

  BestCandidate(const baldr::GraphId& e1, baldr::GraphId& e2, const sif::Cost& c, const uint32_t d)
      : found(false), edgeid(e1), opp_edgeid(e2), cost(c), distance(d), threshold(0) {
  }

  void Update(const baldr::GraphId& e1, baldr::GraphId& e2, const sif::Cost& c, const uint32_t d) {
    edgeid = e1;
    opp_edgeid = e2;
    cost = c;
    distance = d;
  }
};

/**
 * Class to compute cost (cost + time + distance) matrices among locations.
 * This uses a bidirectional search with highway hierarchies. This is a
 * method described by Sebastian Knopp, "Efficient Computation of Many-to-Many
 * Shortest Paths".
 * https://i11www.iti.uni-karlsruhe.de/_media/teaching/theses/files/da-sknopp-06.pdf
 */
class CostMatrix {
public:
  /**
   * Default constructor. Most internal values are set when a query is made so
   * the constructor mainly just sets some internals to a default empty value.
   */
  CostMatrix(const boost::property_tree::ptree& config = {});

  ~CostMatrix();

  /**
   * Forms a time distance matrix from the set of source locations
   * to the set of target locations.
   * @param  source_location_list  List of source/origin locations.
   * @param  target_location_list  List of target/destination locations.
   * @param  graphreader           Graph reader for accessing routing graph.
   * @param  mode_costing          Costing methods.
   * @param  mode                  Travel mode to use.
   * @param  max_matrix_distance   Maximum arc-length distance for current mode.
   */
  void SourceToTarget(Api& request,
                      baldr::GraphReader& graphreader,
                      const sif::mode_costing_t& mode_costing,
                      const sif::travel_mode_t mode,
                      const float max_matrix_distance,
                      const bool has_time = false,
                      const bool invariant = false);

  /**
   * Clear the temporary information generated during time+distance
   * matrix construction.
   */
  void clear();

protected:
  // Access mode used by the costing method
  uint32_t access_mode_;

  // Current travel mode
  sif::TravelMode mode_;

  // Current costing mode
  std::shared_ptr<sif::DynamicCost> costing_;

  uint32_t max_reserved_labels_count_;

  // Number of source and target locations that can be expanded
  uint32_t source_count_;
  uint32_t remaining_sources_;
  uint32_t target_count_;
  uint32_t remaining_targets_;

  // The cost threshold being used for the currently executing query
  float current_cost_threshold_;

  // Status
  std::vector<LocationStatus> source_status_;
  std::vector<LocationStatus> target_status_;

  // Adjacency lists, EdgeLabels, EdgeStatus, and hierarchy limits for each
  // source location (forward traversal)
  std::vector<std::vector<sif::HierarchyLimits>> source_hierarchy_limits_;
  std::vector<baldr::DoubleBucketQueue<sif::BDEdgeLabel>> source_adjacency_;
  std::vector<std::vector<sif::BDEdgeLabel>> source_edgelabel_;
  std::vector<EdgeStatus> source_edgestatus_;

  // Adjacency lists, EdgeLabels, EdgeStatus, and hierarchy limits for each
  // target location (reverse traversal)
  std::vector<std::vector<sif::HierarchyLimits>> target_hierarchy_limits_;
  std::vector<baldr::DoubleBucketQueue<sif::BDEdgeLabel>> target_adjacency_;
  std::vector<std::vector<sif::BDEdgeLabel>> target_edgelabel_;
  std::vector<EdgeStatus> target_edgestatus_;

  // List of best connections found so far
  std::vector<BestCandidate> best_connection_;

  // when doing timezone differencing a timezone cache speeds up the computation
  baldr::DateTime::tz_sys_info_cache_t tz_cache_;

  /**
   * Get the cost threshold based on the current mode and the max arc-length distance
   * for that mode.
   * @param  max_matrix_distance   Maximum arc-length distance for current mode.
   */
  float GetCostThreshold(const float max_matrix_distance);

  /**
   * Form the initial time distance matrix given the sources
   * and destinations.
   * @param  source_location_list   List of source/origin locations.
   * @param  target_location_list   List of target/destination locations.
   */
  void Initialize(const google::protobuf::RepeatedPtrField<valhalla::Location>& source_location_list,
                  const google::protobuf::RepeatedPtrField<valhalla::Location>& target_location_list);

  /**
   * Iterate the forward search from the source/origin location.
   * @param  index        Index of the source location.
   * @param  n            Iteration counter.
   * @param  graphreader  Graph reader for accessing routing graph.
   */
  void ForwardSearch(const uint32_t index,
                     const uint32_t n,
                     baldr::GraphReader& graphreader,
                     const baldr::TimeInfo& time_info,
                     const bool invariant);

  /**
   * Check if the edge on the forward search connects to a reached edge
   * on the reverse search tree.
   * @param  source  Source index.
   * @param  pred    Edge label of the predecessor.
   * @param  n       Iteration counter.
   */
  void CheckForwardConnections(const uint32_t source, const sif::BDEdgeLabel& pred, const uint32_t n);

  /**
   * Update status when a connection is found.
   * @param  source  Source index
   * @param  target  Target index
   */
  void UpdateStatus(const uint32_t source, const uint32_t target);

  /**
   * Iterate the backward search from the target/destination location.
   * @param  index        Index of the target location.
   * @param  graphreader  Graph reader for accessing routing graph.
   */
  void BackwardSearch(const uint32_t index, baldr::GraphReader& graphreader);

  /**
   * Sets the source/origin locations. Search expands forward from these
   * locations.
   * @param  graphreader   Graph reader for accessing routing graph.
   * @param  sources       List of source/origin locations.
   */
  void SetSources(baldr::GraphReader& graphreader,
                  const google::protobuf::RepeatedPtrField<valhalla::Location>& sources,
                  const std::vector<baldr::TimeInfo>& time_infos);

  /**
   * Set the target/destination locations. Search expands backwards from
   * these locations.
   * @param  graphreader   Graph reader for accessing routing graph.
   * @param  targets       List of target locations.
   */
  void SetTargets(baldr::GraphReader& graphreader,
                  const google::protobuf::RepeatedPtrField<valhalla::Location>& targets);

  /**
   * Update destinations along an edge that has been settled (lowest cost path
   * found to the end of edge).
   * @param   origin_index  Index of the origin location.
   * @param   locations     List of locations.
   * @param   destinations  Vector of destination indexes along this edge.
   * @param   edge          Directed edge
   * @param   pred          Predecessor information in shortest path.
   * @param   predindex     Predecessor index in EdgeLabels vector.
   * @return  Returns true if all destinations have been settled.
   */
  bool UpdateDestinations(const uint32_t origin_index,
                          const google::protobuf::RepeatedPtrField<valhalla::Location>& locations,
                          std::vector<uint32_t>& destinations,
                          const baldr::DirectedEdge* edge,
                          const sif::BDEdgeLabel& pred,
                          const uint32_t predindex);

  /**
   * If time awareness was requested for the CostMatrix algorithm, we need
   * to form the paths the sources & targets generated, and recost them to
   * update the best connections, before returning the result.
   * @param   graphreader  Graph tile reader
   * @param   origins      The source locations
   * @param   targets      The target locations
   * @param   time_infos   The time info objects for the sources
   * @param   invariant    Whether time is invariant
   */
  void RecostPaths(baldr::GraphReader& graphreader,
                   google::protobuf::RepeatedPtrField<valhalla::Location>& sources,
                   google::protobuf::RepeatedPtrField<valhalla::Location>& targets,
                   const std::vector<baldr::TimeInfo>& time_infos,
                   bool invariant);

  /**
   * Sets the date_time on the origin locations.
   *
   * @param origins            the origins (sources or targets)
   * @param reader             the reader for looking up timezone information
   * @returns                  time info for each location
   */
  std::vector<baldr::TimeInfo>
  SetOriginTimes(google::protobuf::RepeatedPtrField<valhalla::Location>& origins,
                 baldr::GraphReader& reader) {
    // loop over all locations setting the date time with timezone
    std::vector<baldr::TimeInfo> infos;
    infos.reserve(origins.size());
    for (auto& origin : origins) {
      infos.emplace_back(baldr::TimeInfo::make(origin, reader, &tz_cache_));
    }

    return infos;
  };

private:
  class TargetMap;

  // Mark each target edge with a list of target indexes that have reached it
  std::unique_ptr<TargetMap> targets_;
};

} // namespace thor
} // namespace valhalla

#endif // VALHALLA_THOR_COSTMATRIX_H_
