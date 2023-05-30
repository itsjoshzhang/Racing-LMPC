// Copyright 2023 Haoru Xue
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#ifndef RACING_MPC__RACING_MPC_HPP_
#define RACING_MPC__RACING_MPC_HPP_

#include <memory>

#include <casadi/casadi.hpp>

#include "racing_mpc/racing_mpc_config.hpp"
#include "double_track_planar_model/double_track_planar_model.hpp"

namespace lmpc
{
namespace mpc
{
namespace racing_mpc
{
using lmpc::vehicle_model::base_vehicle_model::BaseVehicleModelConfig;
using lmpc::vehicle_model::double_track_planar_model::DoubleTrackPlanarModel;
using lmpc::vehicle_model::double_track_planar_model::DoubleTrackPlanarModelConfig; \
  using lmpc::vehicle_model::double_track_planar_model::XIndex;
using lmpc::vehicle_model::double_track_planar_model::UIndex;

class RacingMPC
{
public:
  typedef std::shared_ptr<RacingMPC> SharedPtr;
  typedef std::unique_ptr<RacingMPC> UniquePtr;

  explicit RacingMPC(
    RacingMPCConfig::SharedPtr mpc_config,
    DoubleTrackPlanarModel::SharedPtr model);
  const RacingMPCConfig & get_config() const;

  void solve(const casadi::DMDict & in, casadi::DMDict & out);

  void create_warm_start(const casadi::DMDict & in, casadi::DMDict & out);

protected:
  RacingMPCConfig::SharedPtr config_ {};
  DoubleTrackPlanarModel::SharedPtr model_ {};

  casadi::DM scale_x_;
  casadi::DM scale_u_;
  casadi::DM scale_gamma_y_;
  casadi::Function min_time_tracking_cost_;
};
}  // namespace racing_mpc
}  // namespace mpc
}  // namespace lmpc
#endif  // RACING_MPC__RACING_MPC_HPP_