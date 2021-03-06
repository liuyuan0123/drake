#include "drake/automotive/automotive_simulator.h"

#include <stdexcept>

#include <gtest/gtest.h>

#include "drake/automotive/create_trajectory_params.h"
#include "drake/automotive/curve2.h"
#include "drake/automotive/lane_direction.h"
#include "drake/automotive/maliput/api/lane.h"
#include "drake/automotive/maliput/dragway/road_geometry.h"
#include "drake/lcm/drake_mock_lcm.h"
#include "drake/lcmt_driving_command_t.hpp"
#include "drake/lcmt_simple_car_state_t.hpp"
#include "drake/lcmt_viewer_draw.hpp"
#include "drake/lcmt_viewer_load_robot.hpp"
#include "drake/systems/framework/basic_vector.h"
#include "drake/systems/framework/diagram_context.h"
#include "drake/systems/lcm/lcm_publisher_system.h"
#include "drake/systems/lcm/lcm_subscriber_system.h"
#include "drake/systems/rendering/pose_bundle.h"

namespace drake {
namespace automotive {
namespace {

// Simple touches on the getters.
GTEST_TEST(AutomotiveSimulatorTest, BasicTest) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>();
  EXPECT_NE(nullptr, simulator->get_lcm());
  EXPECT_NE(nullptr, simulator->get_builder());
}

// Obtains the serialized version of the last message transmitted on LCM channel
// @p channel into @p result.
void GetLastPublishedSimpleCarState(
    const std::string& channel,
    const lcm::DrakeMockLcm* mock_lcm,
    SimpleCarState<double>* result) {
  const std::vector<uint8_t>& bytes =
      mock_lcm->get_last_published_message(channel);
  drake::lcmt_simple_car_state_t message{};
  const int status = message.decode(bytes.data(), 0, bytes.size());
  if (status < 0) {
    throw std::runtime_error("Failed to decode LCM message simple_car_state.");
  }
  result->set_x(message.x);
  result->set_y(message.y);
  result->set_heading(message.heading);
  result->set_velocity(message.velocity);
}

// Covers AddPriusSimpleCar (and thus AddPublisher), Start, StepBy,
// GetSystemByName.
GTEST_TEST(AutomotiveSimulatorTest, TestPriusSimpleCar) {
  // TODO(jwnimmer-tri) Do something better than "0_" here.
  const std::string kSimpleCarStateChannelName = "0_SIMPLE_CAR_STATE";
  const std::string kCommandChannelName = "DRIVING_COMMAND";

  const std::string driving_command_name =
      systems::lcm::LcmSubscriberSystem::make_name(kCommandChannelName);
  const std::string simple_car_state_name =
      systems::lcm::LcmPublisherSystem::make_name(kSimpleCarStateChannelName);

  // Set up a basic simulation with just a Prius SimpleCar.
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::make_unique<lcm::DrakeMockLcm>());

  const int id = simulator->AddPriusSimpleCar("Foo", kCommandChannelName);
  EXPECT_EQ(id, 0);

  // Grab the systems we want while testing GetBuilderSystemByName() in the
  // process.
  auto& command_sub = dynamic_cast<systems::lcm::LcmSubscriberSystem&>(
      simulator->GetBuilderSystemByName(driving_command_name));
  auto& state_pub = dynamic_cast<systems::lcm::LcmPublisherSystem&>(
      simulator->GetBuilderSystemByName(simple_car_state_name));

  // Finish all initialization, so that we can test the post-init state.
  simulator->Start();

  // Set full throttle.
  drake::lcmt_driving_command_t command{};
  command.acceleration = 11.0;  // Arbitrary large positive.
  lcm::DrakeMockLcm* mock_lcm =
      dynamic_cast<lcm::DrakeMockLcm*>(simulator->get_lcm());
  ASSERT_NE(nullptr, mock_lcm);
  std::vector<uint8_t> message_bytes;
  message_bytes.resize(command.getEncodedSize());
  ASSERT_EQ(command.encode(message_bytes.data(), 0, message_bytes.size()),
            message_bytes.size());
  mock_lcm->InduceSubscriberCallback(kCommandChannelName, &message_bytes[0],
                                     message_bytes.size());

  // Shortly after starting, we should have not have moved much. Take two
  // small steps so that we get a publish a small time after zero (publish
  // occurs at the beginning of a step unless specific publishing times are
  // set).
  simulator->StepBy(0.005);
  simulator->StepBy(0.005);
  SimpleCarState<double> simple_car_state;
  GetLastPublishedSimpleCarState(
      kSimpleCarStateChannelName, mock_lcm, &simple_car_state);
  EXPECT_GT(simple_car_state.x(), 0.0);
  EXPECT_LT(simple_car_state.x(), 0.001);

  // Move a lot.  Confirm that we're moving in +x.
  for (int i = 0; i < 100; ++i) {
    simulator->StepBy(0.01);
  }
  // TODO(jwnimmer-tri) Check the timestamp of the final publication.
  GetLastPublishedSimpleCarState(
      kSimpleCarStateChannelName, mock_lcm, &simple_car_state);
  EXPECT_GT(simple_car_state.x(), 1.0);

  // Confirm that appropriate draw messages are coming out. Just a few of the
  // message's fields are checked.
  const std::string channel_name = "DRAKE_VIEWER_DRAW";
  lcmt_viewer_draw published_draw_message =
      mock_lcm->DecodeLastPublishedMessageAs<lcmt_viewer_draw>(channel_name);

  EXPECT_EQ(published_draw_message.num_links, 1);
  EXPECT_EQ(published_draw_message.link_name.at(0), "car_0::car_origin");

  // The subsystem pointers must not change.
  EXPECT_EQ(&simulator->GetDiagramSystemByName(driving_command_name),
            &command_sub);
  EXPECT_EQ(&simulator->GetDiagramSystemByName(simple_car_state_name),
            &state_pub);
}

// Tests the ability to initialize a SimpleCar to a non-zero initial state.
GTEST_TEST(AutomotiveSimulatorTest, TestPriusSimpleCarInitialState) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::make_unique<lcm::DrakeMockLcm>());
  const double kX{10};
  const double kY{5.5};
  const double kHeading{M_PI_2};
  const double kVelocity{4.5};
  const double kStepSize = 1e-3;

  SimpleCarState<double> initial_state;
  initial_state.set_x(kX);
  initial_state.set_y(kY);
  initial_state.set_heading(kHeading);
  initial_state.set_velocity(kVelocity);

  simulator->AddPriusSimpleCar("My Test Model", "Channel", initial_state);
  simulator->Start();
  simulator->StepBy(kStepSize);

  lcm::DrakeMockLcm* mock_lcm =
      dynamic_cast<lcm::DrakeMockLcm*>(simulator->get_lcm());
  ASSERT_NE(mock_lcm, nullptr);
  const lcmt_simple_car_state_t state_message =
      mock_lcm->DecodeLastPublishedMessageAs<lcmt_simple_car_state_t>(
          "0_SIMPLE_CAR_STATE");

  // Final publish happens at time kStepSize. Since the heading is pi/2, only
  // the y-component of state should be updated.
  EXPECT_EQ(state_message.x, kX);
  EXPECT_EQ(state_message.y, kY + kVelocity * kStepSize);
  EXPECT_EQ(state_message.heading, kHeading);
  EXPECT_EQ(state_message.velocity, kVelocity);
}

GTEST_TEST(AutomotiveSimulatorTest, TestMobilControlledSimpleCar) {
  // Set up a basic simulation with a MOBIL- and IDM-controlled SimpleCar.
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::make_unique<lcm::DrakeMockLcm>());
  lcm::DrakeMockLcm* lcm =
      dynamic_cast<lcm::DrakeMockLcm*>(simulator->get_lcm());
  ASSERT_NE(lcm, nullptr);

  const maliput::api::RoadGeometry* road{};
  EXPECT_NO_THROW(road = simulator->SetRoadGeometry(
      std::make_unique<const maliput::dragway::RoadGeometry>(
          maliput::api::RoadGeometryId("TestDragway"), 2 /* num lanes */,
          100 /* length */, 4 /* lane width */, 1 /* shoulder width */,
          5 /* maximum_height */,
          std::numeric_limits<double>::epsilon() /* linear_tolerance */,
          std::numeric_limits<double>::epsilon() /* angular_tolerance */)));

  // Create one MOBIL car and two stopped cars arranged as follows:
  //
  // ---------------------------------------------------------------
  // ^  +r, +y                                          | Decoy 2 |
  // |    -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -
  // +---->  +s, +x  | MOBIL Car |   | Decoy 1 |
  // ---------------------------------------------------------------
  SimpleCarState<double> simple_car_state;
  simple_car_state.set_x(2);
  simple_car_state.set_y(-2);
  simple_car_state.set_velocity(10);
  const int id_mobil =
      simulator->AddMobilControlledSimpleCar(
          "mobil", true /* with_s */, ScanStrategy::kPath,
          RoadPositionStrategy::kExhaustiveSearch,
          0. /* time period (unused) */, simple_car_state);
  EXPECT_EQ(id_mobil, 0);

  MaliputRailcarState<double> decoy_state;
  decoy_state.set_s(6);
  decoy_state.set_speed(0);
  const int id_decoy1 = simulator->AddPriusMaliputRailcar(
      "decoy1", LaneDirection(road->junction(0)->segment(0)->lane(0)),
      MaliputRailcarParams<double>(), decoy_state);
  EXPECT_EQ(id_decoy1, 1);

  decoy_state.set_s(20);
  const int id_decoy2 = simulator->AddPriusMaliputRailcar(
      "decoy2", LaneDirection(road->junction(0)->segment(0)->lane(1)),
      MaliputRailcarParams<double>(), decoy_state);
  EXPECT_EQ(id_decoy2, 2);

  // Finish all initialization, so that we can test the post-init state.
  simulator->Start();

  // Advances the simulation.
  simulator->StepBy(0.5);

  const lcmt_viewer_draw draw_message =
      lcm->DecodeLastPublishedMessageAs<lcmt_viewer_draw>("DRAKE_VIEWER_DRAW");
  EXPECT_EQ(draw_message.num_links, 3);

  // Expect the SimpleCar to start steering to the left; y value increases.
  const double mobil_y = draw_message.position.at(0).at(1);
  EXPECT_GE(mobil_y, -2.);
}

// Cover AddTrajectoryCar (and thus AddPublisher).
GTEST_TEST(AutomotiveSimulatorTest, TestPriusTrajectoryCar) {
  typedef Curve2<double> Curve2d;
  typedef Curve2d::Point2 Point2d;
  const std::vector<Point2d> waypoints{
      {0.0, 0.0}, {100.0, 0.0},
  };
  const Curve2d curve{waypoints};

  // Set up a basic simulation with a couple Prius TrajectoryCars. Both cars
  // start at position zero; the first has a speed of 1 m/s, while the other is
  // stationary. They both follow a straight 100 m long line.
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::make_unique<lcm::DrakeMockLcm>());
  const int id1 = simulator->AddPriusTrajectoryCar("alice", curve, 1.0, 0.0);
  const int id2 = simulator->AddPriusTrajectoryCar("bob", curve, 0.0, 0.0);
  EXPECT_EQ(id1, 0);
  EXPECT_EQ(id2, 1);

  // Finish all initialization, so that we can test the post-init state.
  simulator->Start();

  // Simulate for one second.
  for (int i = 0; i < 100; ++i) {
    simulator->StepBy(0.01);
  }

  // TODO(jeremy.nimmer) Roughly confirm the car positions are as expected.
}

std::unique_ptr<AutomotiveSimulator<double>> MakeWithIdmCarAndDecoy(
    std::unique_ptr<lcm::DrakeMockLcm> lcm) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::move(lcm));
  const maliput::api::RoadGeometry* road{};
  EXPECT_NO_THROW(
      road = simulator->SetRoadGeometry(
          std::make_unique<const maliput::dragway::RoadGeometry>(
              maliput::api::RoadGeometryId("TestDragway"), 2 /* num lanes */,
              100 /* length */, 4 /* lane width */, 1 /* shoulder width */,
              5 /* maximum_height */,
              std::numeric_limits<double>::epsilon() /* linear_tolerance */,
              std::numeric_limits<double>::epsilon() /* angular_tolerance */)));

  // ---------------------------------------------------------------
  // ^  +r, +y
  // |    -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -  -
  // +---->  +s, +x       |  IDM Car  |         |  Decoy  |
  // ---------------------------------------------------------------
  const double start_s_position(2.);
  const double start_speed(10.);

  const int kStartLaneIndex = 0;
  const maliput::api::Lane* start_lane =
      road->junction(0)->segment(0)->lane(kStartLaneIndex);
  const int kGoalLaneIndex = 0;
  const maliput::api::Lane* goal_lane =
      road->junction(0)->segment(0)->lane(kGoalLaneIndex);

  // Set the initial states.
  const maliput::api::GeoPosition start_position =
      start_lane->ToGeoPosition({start_s_position, 0., 0.});

  SimpleCarState<double> initial_state;
  // The following presumes we are on a dragway, in which x -> s, y -> r.
  initial_state.set_x(start_position.x());
  initial_state.set_y(start_position.y());
  initial_state.set_heading(0.);
  initial_state.set_velocity(start_speed);

  // Expect to throw when given a nullptr Lane.
  EXPECT_THROW(simulator->AddIdmControlledCar(
      "idm_car", true /* with_s */, initial_state, nullptr,
      ScanStrategy::kPath, RoadPositionStrategy::kExhaustiveSearch,
      0. /* time period (unused) */), std::runtime_error);

  int id_idm_car{};
  EXPECT_NO_THROW(id_idm_car = simulator->AddIdmControlledCar(
      "idm_car", true /* with_s */, initial_state, goal_lane,
      ScanStrategy::kPath,
      RoadPositionStrategy::kExhaustiveSearch, 0. /* time period (unused) */));
  EXPECT_EQ(id_idm_car, 0);

  auto dragway = dynamic_cast<const maliput::dragway::RoadGeometry*>(road);
  EXPECT_NE(nullptr, dragway);

  const double traffic_s(6.);
  const double traffic_speed(0.);
  const auto& traffic_params =
      CreateTrajectoryParamsForDragway(*dragway, kStartLaneIndex, traffic_speed,
                                       0. /* start time */);
  const int id_decoy = simulator->AddPriusTrajectoryCar(
      "decoy", std::get<0>(traffic_params), traffic_speed, traffic_s);
  EXPECT_EQ(id_decoy, 1);

  return simulator;
}

// Check the soundness of AddIdmControlledCar.
GTEST_TEST(AutomotiveSimulatorTest, TestIdmControlledSimpleCar) {
  auto simulator =
      MakeWithIdmCarAndDecoy(std::make_unique<lcm::DrakeMockLcm>());

  // Finish all initialization, so that we can test the post-init state.
  simulator->Start();

  // Advances the simulation.
  simulator->StepBy(0.5);

  // Set up LCM and obtain draw messages.
  lcm::DrakeMockLcm* lcm =
      dynamic_cast<lcm::DrakeMockLcm*>(simulator->get_lcm());
  ASSERT_NE(lcm, nullptr);
  const lcmt_viewer_draw draw_message =
      lcm->DecodeLastPublishedMessageAs<lcmt_viewer_draw>("DRAKE_VIEWER_DRAW");
  EXPECT_EQ(draw_message.num_links, 2);

  // Expect the car to start steering to the left; y value increases.
  EXPECT_GE(draw_message.position.at(0).at(0), 0. /* starting x-value */);
  EXPECT_GE(draw_message.position.at(0).at(1), -2. /* starting y-value */);
}

// Check AddIdmControlledCar when LCM has been disabled.
GTEST_TEST(AutomotiveSimulatorTest, TestIdmControlledSimpleCarLcmDisabled) {
  auto simulator = MakeWithIdmCarAndDecoy(nullptr);

  EXPECT_NO_THROW(simulator->Start());

  // Advances the simulation.
  EXPECT_NO_THROW(simulator->StepBy(0.5));
}

// Check that AddIdmControlledCar produces a diagram that is AutoDiff-
// convertible.  Note that the subsystems in both AddIdmControlledCar and
// AddPriusTrajectoryCar must be AutoDiff supported.
//
// TODO(jadecastro) Consider checking the autodiff derivatives of the autodiff-
// converted diagram.
GTEST_TEST(AutomotiveSimulatorTest, TestIdmControlledSimpleCarAutoDiff) {
  // Set up a basic simulation with an IDM-controlled SimpleCar with LCM
  // disabled.
  auto simulator = MakeWithIdmCarAndDecoy(nullptr);

  simulator->Build();

  const auto& plant = simulator->GetDiagram();
  auto plant_simulator =
      std::make_unique<systems::Simulator<double>>(plant);

  plant_simulator->AdvanceTo(0.5);

  // Converts to AutoDiffXd.
  auto plant_ad = plant.ToAutoDiffXd();
  auto plant_ad_simulator =
      std::make_unique<systems::Simulator<AutoDiffXd>>(*plant_ad);

  plant_ad_simulator->AdvanceTo(0.5);
}

// Returns the x-position of the vehicle based on an lcmt_viewer_draw message.
// It also checks that the y-position of the vehicle is equal to the provided y
// value.
double GetPosition(const lcmt_viewer_draw& message, double y) {
  EXPECT_EQ(message.num_links, 1);
  EXPECT_EQ(message.link_name.at(0), "car_0::car_origin");
  EXPECT_DOUBLE_EQ(message.position.at(0).at(1), y);
  return message.position.at(0).at(0);
}

// Covers AddMaliputRailcar().
GTEST_TEST(AutomotiveSimulatorTest, TestMaliputRailcar) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::make_unique<lcm::DrakeMockLcm>());
  lcm::DrakeMockLcm* lcm =
      dynamic_cast<lcm::DrakeMockLcm*>(simulator->get_lcm());
  ASSERT_NE(lcm, nullptr);
  const double kR{0.5};
  MaliputRailcarParams<double> params;
  params.set_r(kR);

  EXPECT_THROW(simulator->AddPriusMaliputRailcar("foo", LaneDirection()),
               std::runtime_error);

  const maliput::api::RoadGeometry* road{};
  EXPECT_NO_THROW(
      road = simulator->SetRoadGeometry(
          std::make_unique<const maliput::dragway::RoadGeometry>(
              maliput::api::RoadGeometryId("TestDragway"), 1 /* num lanes */,
              100 /* length */, 4 /* lane width */, 1 /* shoulder width */,
              5 /* maximum_height */,
              std::numeric_limits<double>::epsilon() /* linear_tolerance */,
              std::numeric_limits<double>::epsilon() /* angular_tolerance */)));

  EXPECT_THROW(
      simulator->AddPriusMaliputRailcar("bar", LaneDirection(), params),
      std::runtime_error);

  const auto different_road =
      std::make_unique<const maliput::dragway::RoadGeometry>(
          maliput::api::RoadGeometryId("DifferentDragway"), 2 /* num lanes */,
          50 /* length */, 3 /* lane width */, 2 /* shoulder width */,
          5 /* maximum_height */,
          std::numeric_limits<double>::epsilon() /* linear_tolerance */,
          std::numeric_limits<double>::epsilon() /* angular_tolerance */);

  EXPECT_THROW(simulator->AddPriusMaliputRailcar(
                   "bar", LaneDirection(
                              different_road->junction(0)->segment(0)->lane(0)),
                   params),
               std::runtime_error);

  const int id = simulator->AddPriusMaliputRailcar(
      "model_name", LaneDirection(road->junction(0)->segment(0)->lane(0)),
      params, MaliputRailcarState<double>() /* initial state */);
  EXPECT_EQ(id, 0);

  simulator->Start();

  // AutomotiveSimulator's call to ConnectToDrakeVisualizer causes LCM draw
  // messages to be published every 1/60s (starting at time zero). If that
  // rate is changed, the step size here will need to be changed to match.
  const double step_size = 1.0/60;
  simulator->StepBy(step_size);

  const double initial_x = 0.0;

  // Verifies the acceleration is zero even if
  // AutomotiveSimulator::SetMaliputRailcarAccelerationCommand() was not called.
  const lcmt_viewer_draw draw_message0 =
      lcm->DecodeLastPublishedMessageAs<lcmt_viewer_draw>("DRAKE_VIEWER_DRAW");
  // The following tolerance was determined empirically.
  EXPECT_NEAR(GetPosition(draw_message0, kR), initial_x, 1e-4);

  // Sets the commanded acceleration to be zero.
  simulator->SetMaliputRailcarAccelerationCommand(id, 0);
  simulator->StepBy(step_size);

  // Verifies that the vehicle hasn't moved yet. This is expected since the
  // commanded acceleration is zero.
  const lcmt_viewer_draw draw_message1 =
      lcm->DecodeLastPublishedMessageAs<lcmt_viewer_draw>("DRAKE_VIEWER_DRAW");
  // The following tolerance was determined empirically.
  EXPECT_NEAR(GetPosition(draw_message1, kR), initial_x, 1e-4);

  // Sets the commanded acceleration to be 10 m/s^2.
  simulator->SetMaliputRailcarAccelerationCommand(id, 10);

  // Advances the simulation to allow the MaliputRailcar to begin accelerating.
  simulator->StepBy(step_size);

  // Verifies that the MaliputRailcar has moved forward relative to prior to
  // the nonzero acceleration command being issued.
  const lcmt_viewer_draw draw_message2 =
      lcm->DecodeLastPublishedMessageAs<lcmt_viewer_draw>("DRAKE_VIEWER_DRAW");
  EXPECT_LT(draw_message1.position.at(0).at(0), GetPosition(draw_message2, kR));
}

// Verifies correct LCM messages being published by the Diagram.
GTEST_TEST(AutomotiveSimulatorTest, TestLcmOutput) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::make_unique<lcm::DrakeMockLcm>());

  simulator->AddPriusSimpleCar("Model1", "Channel1");
  simulator->AddPriusSimpleCar("Model2", "Channel2");

  typedef Curve2<double> Curve2d;
  typedef Curve2d::Point2 Point2d;
  const std::vector<Point2d> waypoints{Point2d{0, 0}, Point2d{1, 0}};
  const Curve2d curve{waypoints};
  simulator->AddPriusTrajectoryCar("alice", curve, 1 /* speed */,
                                   0 /* start time */);
  simulator->AddPriusTrajectoryCar("bob", curve, 1 /* speed */,
                                   0 /* start time */);

  simulator->Start();
  simulator->StepBy(1e-3);

  const lcm::DrakeLcmInterface* lcm = simulator->get_lcm();
  ASSERT_NE(lcm, nullptr);

  const lcm::DrakeMockLcm* mock_lcm =
      dynamic_cast<const lcm::DrakeMockLcm*>(lcm);
  ASSERT_NE(mock_lcm, nullptr);

  const int expected_num_links = 4;

  // Verifies that an lcmt_viewer_load_robot message was transmitted.
  const lcmt_viewer_load_robot load_message =
      mock_lcm->DecodeLastPublishedMessageAs<lcmt_viewer_load_robot>(
          "DRAKE_VIEWER_LOAD_ROBOT");
  EXPECT_EQ(load_message.num_links, expected_num_links);

  // Verifies that an lcmt_viewer_draw message was transmitted.
  const lcmt_viewer_draw draw_message =
      mock_lcm->DecodeLastPublishedMessageAs<lcmt_viewer_draw>(
          "DRAKE_VIEWER_DRAW");
  EXPECT_EQ(load_message.num_links, expected_num_links);
}

// Verifies that exceptions are thrown if a vehicle with a non-unique name is
// added to the simulation.
GTEST_TEST(AutomotiveSimulatorTest, TestDuplicateVehicleNameException) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::make_unique<lcm::DrakeMockLcm>());

  EXPECT_NO_THROW(simulator->AddPriusSimpleCar("Model1", "Channel1"));
  EXPECT_THROW(simulator->AddPriusSimpleCar("Model1", "foo"),
               std::runtime_error);

  typedef Curve2<double> Curve2d;
  typedef Curve2d::Point2 Point2d;
  const std::vector<Point2d> waypoints{Point2d{0, 0}, Point2d{1, 0}};
  const Curve2d curve{waypoints};

  EXPECT_NO_THROW(simulator->AddPriusTrajectoryCar(
      "alice", curve, 1 /* speed */, 0 /* start time */));
  EXPECT_THROW(simulator->AddPriusTrajectoryCar("alice", curve, 1 /* speed */,
                                                0 /* start time */),
               std::runtime_error);
  EXPECT_THROW(simulator->AddPriusTrajectoryCar("Model1", curve, 1 /* speed */,
                                                0 /* start time */),
               std::runtime_error);

  const MaliputRailcarParams<double> params;
  const maliput::api::RoadGeometry* road{};
  EXPECT_NO_THROW(
      road = simulator->SetRoadGeometry(
          std::make_unique<const maliput::dragway::RoadGeometry>(
              maliput::api::RoadGeometryId("TestDragway"), 1 /* num lanes */,
              100 /* length */, 4 /* lane width */, 1 /* shoulder width */,
              5 /* maximum_height */,
              std::numeric_limits<double>::epsilon() /* linear_tolerance */,
              std::numeric_limits<double>::epsilon() /* angular_tolerance */)));
  EXPECT_NO_THROW(simulator->AddPriusMaliputRailcar(
      "Foo", LaneDirection(road->junction(0)->segment(0)->lane(0)), params,
      MaliputRailcarState<double>() /* initial state */));
  EXPECT_THROW(
      simulator->AddPriusMaliputRailcar(
          "alice", LaneDirection(road->junction(0)->segment(0)->lane(0)),
          params, MaliputRailcarState<double>() /* initial state */),
      std::runtime_error);
  EXPECT_THROW(
      simulator->AddPriusMaliputRailcar(
          "Model1", LaneDirection(road->junction(0)->segment(0)->lane(0)),
          params, MaliputRailcarState<double>() /* initial state */),
      std::runtime_error);
}

// Verifies that no exception is thrown when multiple IDM-controlled
// MaliputRailcar vehicles are simulated. This prevents a regression of #5886.
GTEST_TEST(AutomotiveSimulatorTest, TestIdmControllerUniqueName) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::make_unique<lcm::DrakeMockLcm>());

  const MaliputRailcarParams<double> params;
  const maliput::api::RoadGeometry* road = simulator->SetRoadGeometry(
      std::make_unique<const maliput::dragway::RoadGeometry>(
          maliput::api::RoadGeometryId("TestDragway"), 1 /* num lanes */,
          100 /* length */, 4 /* lane width */, 1 /* shoulder width */,
          5 /* maximum_height */,
          std::numeric_limits<double>::epsilon() /* linear_tolerance */,
          std::numeric_limits<double>::epsilon() /* angular_tolerance */));
  simulator->AddIdmControlledPriusMaliputRailcar(
      "Alice", LaneDirection(road->junction(0)->segment(0)->lane(0)),
      ScanStrategy::kPath, RoadPositionStrategy::kExhaustiveSearch,
      0. /* time period (unused) */,
      params, MaliputRailcarState<double>() /* initial state */);
  simulator->AddIdmControlledPriusMaliputRailcar(
      "Bob", LaneDirection(road->junction(0)->segment(0)->lane(0)),
      ScanStrategy::kPath, RoadPositionStrategy::kExhaustiveSearch,
      0. /* time period (unused) */,
      params, MaliputRailcarState<double>() /* initial state */);

  EXPECT_NO_THROW(simulator->Start());
}

// Verifies that the velocity outputs of the MaliputRailcars are connected to
// the PoseAggregator, which prevents a regression of #5894.
GTEST_TEST(AutomotiveSimulatorTest, TestRailcarVelocityOutput) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>(
      std::make_unique<lcm::DrakeMockLcm>());

  const MaliputRailcarParams<double> params;
  const maliput::api::RoadGeometry* road =
      simulator->SetRoadGeometry(
          std::make_unique<const maliput::dragway::RoadGeometry>(
              maliput::api::RoadGeometryId("TestDragway"), 1 /* num lanes */,
              100 /* length */, 4 /* lane width */, 1 /* shoulder width */,
              5 /* maximum_height */,
              std::numeric_limits<double>::epsilon() /* linear_tolerance */,
              std::numeric_limits<double>::epsilon() /* angular_tolerance */));
  MaliputRailcarState<double> alice_initial_state;
  alice_initial_state.set_s(5);
  alice_initial_state.set_speed(1);
  const int alice_id = simulator->AddPriusMaliputRailcar("Alice",
      LaneDirection(road->junction(0)->segment(0)->lane(0)), params,
      alice_initial_state);
  const int bob_id = simulator->AddIdmControlledPriusMaliputRailcar("Bob",
      LaneDirection(road->junction(0)->segment(0)->lane(0)),
      ScanStrategy::kPath, RoadPositionStrategy::kExhaustiveSearch,
      0. /* time period (unused) */, params,
      MaliputRailcarState<double>() /* initial state */);

  EXPECT_NO_THROW(simulator->Start());

  // Advances the simulation to allow Alice's MaliputRailcar to move at fixed
  // speed and Bob's MaliputRailcar to move under IDM control.
  simulator->StepBy(1);

  const int kAliceIndex{0};
  const int kBobIndex{1};

  // Verifies that the velocity within the PoseAggregator's PoseBundle output is
  // non-zero.
  const systems::rendering::PoseBundle<double> poses =
      simulator->GetCurrentPoses();
  ASSERT_EQ(poses.get_num_poses(), 2);
  ASSERT_EQ(poses.get_model_instance_id(kAliceIndex), alice_id);
  ASSERT_EQ(poses.get_model_instance_id(kBobIndex), bob_id);
  EXPECT_FALSE(poses.get_velocity(kAliceIndex).get_value().isZero());
  EXPECT_FALSE(poses.get_velocity(kBobIndex).get_value().isZero());
}

// Tests Build/Start logic
GTEST_TEST(AutomotiveSimulatorTest, TestBuild) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>();

  simulator->AddPriusSimpleCar("Model1", "Channel1");
  simulator->AddPriusSimpleCar("Model2", "Channel2");

  simulator->Build();
  EXPECT_FALSE(simulator->has_started());
  EXPECT_NO_THROW(simulator->GetDiagram());

  simulator->Start(0.0);
  EXPECT_TRUE(simulator->has_started());
  EXPECT_NO_THROW(simulator->GetDiagram());
}

// Tests Build/Start logic (calling Start only)
GTEST_TEST(AutomotiveSimulatorTest, TestBuild2) {
  auto simulator = std::make_unique<AutomotiveSimulator<double>>();

  simulator->AddPriusSimpleCar("Model1", "Channel1");
  simulator->AddPriusSimpleCar("Model2", "Channel2");

  simulator->Start(0.0);
  EXPECT_NO_THROW(simulator->GetDiagram());
}

}  // namespace
}  // namespace automotive
}  // namespace drake
