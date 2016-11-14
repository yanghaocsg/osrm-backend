#include "extractor/guidance/mergable_road_detector.hpp"
#include "extractor/guidance/node_based_graph_walker.hpp"

namespace osrm
{
namespace extractor
{
namespace guidance
{

namespace
{

// check a connected road for equality of a name
inline auto makeCheckRoadForName(const NameID name_id,
                                 const util::NodeBasedDynamicGraph &node_based_graph)
{
    return [name_id, &node_based_graph](const ConnectedRoad &road) {
        return name_id == node_based_graph.GetEdgeData(road.eid).name_id;
    };
}
}

MergableRoadDetector::MergableRoadDetector(const util::NodeBasedDynamicGraph &node_based_graph,
                                           const std::vector<QueryNode> &node_coordinates,
                                           const IntersectionGenerator &intersection_generator,
                                           const CoordinateExtractor &coordinate_extractor)
    : node_based_graph(node_based_graph), node_coordinates(node_coordinates),
      intersection_generator(intersection_generator), coordinate_extractor(coordinate_extractor)
{
}

bool MergableRoadDetector::CanMergeRoad(const NodeID intersection_node,
                                        const ConnectedRoad &lhs,
                                        const ConnectedRoad &rhs) const
{
    // mergable roads cannot hide a turn. We are not allowed to remove any of them
    if (lhs.entry_allowed && rhs.entry_allowed)
        return false;

    if (angularDeviation(lhs.angle, rhs.angle) > 60)
        return false;

    const auto &lhs_edge_data = node_based_graph.GetEdgeData(lhs.eid);
    const auto &rhs_edge_data = node_based_graph.GetEdgeData(rhs.eid);

    // roundabouts are special, simply don't hurt them. We might not want to bear the
    // consequences
    if (lhs_edge_data.roundabout || rhs_edge_data.roundabout)
        return false;

    // and they need to describe the same road
    if (!RoadDataIsCompatible(lhs_edge_data, rhs_edge_data))
        return false;

    // don't use any circular links, since they mess up detection we jump out early.
    //
    //          / -- \
    // a ---- b - - /
    const auto road_target = [this](const ConnectedRoad &road) {
        return node_based_graph.GetTarget(road.eid);
    };
    // TODO might have to skip over trivial intersections
    if (road_target(lhs) == intersection_node || road_target(lhs) == intersection_node)
        return false;

    std::cout << "Comparison:\n"
              << "\tLink Road: " << IsLinkRoad(intersection_node,lhs) << " " << IsLinkRoad(intersection_node,rhs) << "\n"
              << "\tTriangls: " << IsNarrowTriangle(intersection_node,lhs,rhs) << "\n"
              << "\tParallel: " << HaveSameDirection(intersection_node,lhs,rhs) << std::endl;

    // Don't merge link roads
    if (IsLinkRoad(intersection_node, lhs) || IsLinkRoad(intersection_node, rhs))
        return false;

    if (IsNarrowTriangle(intersection_node, lhs, rhs))
        return true;

    if (HaveSameDirection(intersection_node, lhs, rhs))
        return true;

    // if (connectAgain(intersection_node, lhs, rhs, node_based_graph, intersection_generator))

    return false;
    // finally check if two roads describe the same way
}

bool MergableRoadDetector::RoadDataIsCompatible(const util::NodeBasedEdgeData &lhs_edge_data,
                                                const util::NodeBasedEdgeData &rhs_edge_data) const
{
    // to describe the same road, but in opposite directions (which is what we require for a
    // merge), the roads have to feature one reversed and one non-reversed edge
    if (lhs_edge_data.reversed == rhs_edge_data.reversed)
        return false;

    // The travel mode should be the same for both roads. If we were to merge different travel
    // modes, we would hide information/run the risk of loosing valid choices (e.g. short period
    // of pushing)
    if (lhs_edge_data.travel_mode != rhs_edge_data.travel_mode)
        return false;

    return lhs_edge_data.road_classification == rhs_edge_data.road_classification;
}

bool MergableRoadDetector::IsNarrowTriangle(const NodeID intersection_node,
                                            const ConnectedRoad &lhs,
                                            const ConnectedRoad &rhs) const
{
    // selection data to the right and left
    IntersectionFinderAccumulator left_accumulator(5, intersection_generator),
        right_accumulator(5, intersection_generator);

    // Standard following the straightmost road
    // Since both items have the same id, we can `select` based on any setup
    SelectStraightmostRoadByNameAndOnlyChoice selector(
        node_based_graph.GetEdgeData(lhs.eid).name_id, false);

    NodeBasedGraphWalker graph_walker(node_based_graph, intersection_generator);
    graph_walker.TraverseRoad(intersection_node, lhs.eid, left_accumulator, selector);
    // if the intersection does not have a right turn, we continue onto the next one once
    // (skipping over a single small side street)
    if (angularDeviation(left_accumulator.intersection.findClosestTurn(90)->angle, 90) >
        NARROW_TURN_ANGLE)
    {
        graph_walker.TraverseRoad(node_based_graph.GetTarget(left_accumulator.via_edge_id),
                                  left_accumulator.intersection.findClosestTurn(180)->eid,
                                  left_accumulator,
                                  selector);
    }
    graph_walker.TraverseRoad(intersection_node, rhs.eid, right_accumulator, selector);
    if (angularDeviation(right_accumulator.intersection.findClosestTurn(270)->angle, 270) >
        NARROW_TURN_ANGLE)
    {
        graph_walker.TraverseRoad(node_based_graph.GetTarget(right_accumulator.via_edge_id),
                                  right_accumulator.intersection.findClosestTurn(180)->eid,
                                  right_accumulator,
                                  selector);
    }

    BOOST_ASSERT(!left_accumulator.intersection.empty() && !right_accumulator.intersection.empty());

    // find the closes resembling a right turn
    const auto connector_turn = left_accumulator.intersection.findClosestTurn(90);
    // check if that right turn connects to the right_accumulator intersection (i.e. we have a
    // triangle)
    // a connection should be somewhat to the right, when looking at the left side of the
    // triangle
    //
    //    b ..... c
    //     \     /
    //      \   /
    //       \ /
    //        a
    //
    // e.g. here when looking at `a,b`, a narrow triangle should offer a turn to the right, when
    // we
    // want to connect to c
    if (angularDeviation(connector_turn->angle, 90) > NARROW_TURN_ANGLE)
        return false;

    const auto num_lanes = [this](const ConnectedRoad &road) {
        return std::max<std::uint8_t>(
            node_based_graph.GetEdgeData(road.eid).road_classification.GetNumberOfLanes(), 1);
    };

    // the width we can bridge at the intersection
    const auto assumed_lane_width = (num_lanes(lhs) + num_lanes(rhs)) * 3.25;

    if (util::coordinate_calculation::haversineDistance(
            node_coordinates[node_based_graph.GetTarget(left_accumulator.via_edge_id)],
            node_coordinates[node_based_graph.GetTarget(right_accumulator.via_edge_id)]) >
        (assumed_lane_width + 8))
        return false;

    // check if both intersections are connected
    IntersectionFinderAccumulator connect_accumulator(5, intersection_generator);
    graph_walker.TraverseRoad(node_based_graph.GetTarget(left_accumulator.via_edge_id),
                              connector_turn->eid,
                              connect_accumulator,
                              selector);

    // the if both items are connected
    return node_based_graph.GetTarget(connect_accumulator.via_edge_id) ==
           node_based_graph.GetTarget(right_accumulator.via_edge_id);
}

bool MergableRoadDetector::HaveSameDirection(const NodeID intersection_node,
                                             const ConnectedRoad &lhs,
                                             const ConnectedRoad &rhs) const
{
    if (angularDeviation(lhs.angle, rhs.angle) > 90)
        return false;

    // Find a coordinate following a road that is far away
    NodeBasedGraphWalker graph_walker(node_based_graph, intersection_generator);
    const auto getCoordinatesAlongWay = [&](const EdgeID edge_id, const double max_length) {
        LengthLimitedCoordinateAccumulator accumulator(
            coordinate_extractor, node_based_graph, max_length);
        SelectStraightmostRoadByNameAndOnlyChoice selector(
            node_based_graph.GetEdgeData(edge_id).name_id, false);
        graph_walker.TraverseRoad(intersection_node, edge_id, accumulator, selector);
        return std::make_pair(accumulator.accumulated_length, accumulator.coordinates);
    };

    std::vector<util::Coordinate> coordinates_to_the_left, coordinates_to_the_right;
    double distance_traversed_to_the_left, distance_traversed_to_the_right;

    const double constexpr distance_to_extract = 100;

    std::tie(distance_traversed_to_the_left, coordinates_to_the_left) =
        getCoordinatesAlongWay(lhs.eid, distance_to_extract);

    std::cout << "Travelled Left: " << distance_traversed_to_the_left << std::endl;
    // quit early if the road is not very long
    if (distance_traversed_to_the_left <= 40)
        return false;

    coordinates_to_the_left = coordinate_extractor.SampleCoordinates(
        std::move(coordinates_to_the_left), distance_to_extract, 5);

    std::tie(distance_traversed_to_the_right, coordinates_to_the_right) =
        getCoordinatesAlongWay(rhs.eid, distance_to_extract);
    std::cout << "Travelled Right: " << distance_traversed_to_the_right << std::endl;
    if (distance_traversed_to_the_right <= 40)
        return false;


    coordinates_to_the_right = coordinate_extractor.SampleCoordinates(
        std::move(coordinates_to_the_right), distance_to_extract, 5);
    // extract the number of lanes for a road
    const auto num_lanes = [this](const ConnectedRoad &road) {
        return std::max<std::uint8_t>(
            node_based_graph.GetEdgeData(road.eid).road_classification.GetNumberOfLanes(), 1);
    };

    // we allow some basic deviation for all roads. If the there are more lanes present, we
    // allow
    // for a bit more deviation
    const auto max_deviation = [&]() {
        const auto lane_count = std::max<std::uint8_t>(2, std::max(num_lanes(lhs), num_lanes(rhs)));
        return 4 * sqrt(lane_count);
    }();

    const auto are_parallel = util::coordinate_calculation::areParallel(
        coordinates_to_the_left, coordinates_to_the_right, max_deviation);

    return are_parallel;
}

bool MergableRoadDetector::ConnectAgain(const NodeID intersection_node,
                                        const ConnectedRoad &lhs,
                                        const ConnectedRoad &rhs) const
{
    // compute the set of all intersection_nodes along the way of an edge, until it reaches a
    // location with the same name repeatet at least three times
    const auto findMeetUpCandidate = [&](const NameID searched_name, const ConnectedRoad &road) {
        auto current_node = intersection_node;
        auto current_eid = road.eid;

        const auto has_requested_name = makeCheckRoadForName(searched_name, node_based_graph);
        // limit our search to at most 10 intersections. This is intended to ignore connections
        // that
        // are really far away
        for (std::size_t hop_count = 0; hop_count < 10; ++hop_count)
        {
            const auto next_intersection =
                intersection_generator.GetConnectedRoads(current_node, current_eid);
            const auto count = std::count_if(
                next_intersection.begin() + 1, next_intersection.end(), has_requested_name);

            if (count >= 2)
                return node_based_graph.GetTarget(current_eid);
            else if (count == 0)
            {
                return SPECIAL_NODEID;
            }
            else
            {
                current_node = node_based_graph.GetTarget(current_eid);
                // skip over bridges/similar
                if (next_intersection.size() == 2)
                    current_eid = next_intersection[1].eid;
                else
                {
                    const auto next_turn = std::find_if(
                        next_intersection.begin() + 1, next_intersection.end(), has_requested_name);

                    if (angularDeviation(next_turn->angle, 180) > NARROW_TURN_ANGLE)
                        return current_node;
                    BOOST_ASSERT(next_turn != next_intersection.end());
                    current_eid = next_turn->eid;
                }
            }
        }

        return SPECIAL_NODEID;
    };

    const auto left_candidate =
        findMeetUpCandidate(node_based_graph.GetEdgeData(lhs.eid).name_id, lhs);
    const auto right_candidate =
        findMeetUpCandidate(node_based_graph.GetEdgeData(rhs.eid).name_id, rhs);

    return left_candidate == right_candidate && left_candidate != SPECIAL_NODEID &&
           left_candidate != intersection_node;
}

bool MergableRoadDetector::IsLinkRoad(const NodeID intersection_node,
                                      const ConnectedRoad &road) const
{
    // selection data to the right and left
    IntersectionFinderAccumulator accumulator(2, intersection_generator);

    // Standard following the straightmost road
    // Since both items have the same id, we can `select` based on any setup
    SelectStraightmostRoadByNameAndOnlyChoice selector(
        node_based_graph.GetEdgeData(road.eid).name_id, false);

    NodeBasedGraphWalker graph_walker(node_based_graph, intersection_generator);
    graph_walker.TraverseRoad(intersection_node, road.eid, accumulator, selector);

    const auto &next_intersection_along_road = accumulator.intersection;
    const auto extract_name = [this](const ConnectedRoad &road) {
        return node_based_graph.GetEdgeData(road.eid).name_id;
    };

    const auto requested_name = extract_name(road);
    const auto next_road_along_path =
        findClosestTurn(accumulator.intersection,
                        STRAIGHT_ANGLE,
                        [requested_name, extract_name](const ConnectedRoad &compare_road) {
                            return requested_name != extract_name(compare_road);
                        });

    // we need to have a continuing road to successfully detect a link road
    if (next_road_along_path == accumulator.intersection.end())
        return false;

    const auto opposite_of_next_road_along_path = next_intersection_along_road.findClosestTurn(
        restrictAngleToValidRange(next_road_along_path->angle + 180));

    // we cannot be looking at the same road we came from
    if (node_based_graph.GetTarget(opposite_of_next_road_along_path->eid) == accumulator.nid)
        return false;

    // near straight road that continues
    return angularDeviation(opposite_of_next_road_along_path->angle, next_road_along_path->angle) >=
               160 &&
           RoadDataIsCompatible(
               node_based_graph.GetEdgeData(next_road_along_path->eid),
               node_based_graph.GetEdgeData(opposite_of_next_road_along_path->eid));
}

} // namespace guidance
} // namespace extractor
} // namespace osrm
