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

#include "double_track_planar_model/double_track_planar_model.hpp"
#include "lmpc_utils/utils.hpp"
#define GRAVITY 9.8

namespace lmpc
{
namespace vehicle_model
{
namespace double_track_planar_model
{
DoubleTrackPlanarModel::DoubleTrackPlanarModel(
  base_vehicle_model::BaseVehicleModelConfig::SharedPtr base_config,
  DoubleTrackPlanarModelConfig::SharedPtr config)
: base_vehicle_model::BaseVehicleModel(base_config), config_(config)
{
  compile_dynamics();
}

const DoubleTrackPlanarModelConfig & DoubleTrackPlanarModel::get_config() const
{
  return *config_.get();
}

size_t DoubleTrackPlanarModel::nx() const
{
  return 6;
}

size_t DoubleTrackPlanarModel::nu() const
{
  return 3;
}

void DoubleTrackPlanarModel::forward_dynamics(const casadi::MXDict & in, casadi::MXDict & out)
{
  const auto gamma_y = casadi::MX::sym("gamma_y", 1);

  auto dyn_in = casadi::MXDict(in);
  dyn_in["gamma_y"] = gamma_y;
  out = dynamics_(dyn_in);

  const auto Fx_ij = out.at("Fx_ij");
  const auto Fy_ij = out.at("Fy_ij");

  const auto gamma_y_solve = lateral_load_transfer_(casadi::MX{0.0});
  for (auto & var : out) {
    var.second = substitute(var.second, gamma_y, gamma_y_solve[0]);
  }
  out["gamma_y"] = gamma_y_solve[0];
}

void DoubleTrackPlanarModel::add_nlp_constraints(casadi::Opti & opti, const casadi::MXDict & in)
{
  // using casadi::MX;
  using TI = lmpc::utils::TyreIndex;
  const auto & x = in.at("x");
  const auto & u = in.at("u");
  const auto & gamma_y = in.at("gamma_y");
  const auto & xip1 = in.at("xip1");
  const auto & uip1 = in.at("uip1");
  const auto & t = in.at("t");

  const auto & v = x(5);
  const auto & fd = u(0);
  const auto & fb = u(1);
  const auto & delta = u(2);

  const auto & twf = get_base_config().chassis_config->tw_f;
  const auto & twr = get_base_config().chassis_config->tw_r;
  const auto & delta_max = get_base_config().steer_config->max_steer;
  const auto & hcog = get_base_config().chassis_config->cg_height;
  const auto & mu = get_config().mu;
  const auto & P_max = get_config().P_max;
  const auto & Fd_max = get_config().Fd_max;
  const auto & Fb_max = get_config().Fb_max;
  const auto & Td = get_config().Td;
  const auto & Tb = get_config().Tb;
  const auto & Tdelta = get_base_config().steer_config->max_steer /
    get_base_config().steer_config->max_steer_rate;

  // dynamics constraint
  auto xip1_temp = casadi::MX(xip1);
  xip1_temp(2) = lmpc::utils::align_yaw<casadi::MX>(xip1_temp(2), x(2));
  const auto out1 = dynamics_({{"x", x}, {"u", u}, {"gamma_y", gamma_y}});
  const auto f1 = out1.at("x_dot");
  const auto out2 = dynamics_({{"x", xip1_temp}, {"u", u}, {"gamma_y", gamma_y}});
  const auto f2 = out2.at("x_dot");
  const auto xm = 0.5 * (x + xip1_temp) + (t / 8.0) * (f1.T() - f2.T());
  const auto outm = dynamics_({{"x", xm}, {"u", u}, {"gamma_y", gamma_y}});
  const auto fm = outm.at("x_dot");
  opti.subject_to(x + (t / 6.0) * (f1.T() + 4 * fm.T() + f2.T()) - xip1_temp == 0);

  // tyre constraints
  const auto Fx_ij = out1.at("Fx_ij");
  const auto Fy_ij = out1.at("Fy_ij");
  const auto Fz_ij = out1.at("Fz_ij");
  for (int i = 0; i < 4; i++) {
    opti.subject_to(pow(Fx_ij(i) / (mu * Fz_ij(i)), 2) + pow(Fy_ij(i) / (mu * Fz_ij(i)), 2) <= 1);
  }

  // load transfer constraint
  opti.subject_to(
    gamma_y ==
    hcog / (0.5 * (twf + twr)) *
    (Fy_ij(TI::RL) + Fy_ij(TI::RR) + (Fx_ij(TI::FL) + Fx_ij(TI::FR)) * sin(delta) +
    (Fy_ij(TI::FL) + Fy_ij(TI::FR)) * cos(delta)));

  // static actuator cconstraint
  opti.subject_to(v * fd <= P_max);
  opti.subject_to(v >= 0.0);
  opti.subject_to(opti.bounded(0.0, fd, Fd_max));
  opti.subject_to(opti.bounded(Fb_max, fb, 0.0));
  opti.subject_to(pow(fd * fb, 2) <= 1.0);
  opti.subject_to(opti.bounded(-1.0 * delta_max, delta, delta_max));

  // dynamic actuator constraint
  opti.subject_to((uip1(0) - fd) / t <= Fd_max / Td);
  opti.subject_to((uip1(1) - fb) / t >= Fb_max / Tb);
  opti.subject_to(opti.bounded(-delta_max / Tdelta, (uip1(2) - delta) / t, delta_max / Tdelta));
}

void DoubleTrackPlanarModel::compile_dynamics()
{
  using casadi::SX;

  auto x = SX::sym("x", nx());
  auto u = SX::sym("u", nu());
  auto gamma_y = SX::sym("gamma_y", 1);   // lateral load transfer

  const auto & phi = x(2);  // yaw
  const auto & omega = x(3);  // yaw rate
  const auto & beta = x(4);  // slip angle
  const auto & v = x(5);  // velocity magnitude
  const auto & fd = u(0);  // drive force
  const auto & fb = u(1);  // brake forcce
  const auto & delta = u(2);  // front wheel angle
  const auto & v_sq = pow(v, 2);

  const auto & kd_f = get_base_config().powertrain_config->kd;
  const auto & kb_f = get_base_config().front_brake_config->bias;  // front brake force bias
  const auto & m = get_base_config().chassis_config->total_mass;  // mass of car
  const auto & Jzz = get_base_config().chassis_config->moi;  // MOI around z axis
  const auto & l = get_base_config().chassis_config->wheel_base;  // wheelbase
  const auto & lf = get_base_config().chassis_config->cg_ratio * l;  // cg to front axle
  const auto lr = l - lf;  // cg to rear axle
  const auto & twf = get_base_config().chassis_config->tw_f;  // front track width
  const auto & twr = get_base_config().chassis_config->tw_r;  // rear track width
  const auto & fr = get_base_config().chassis_config->fr;  // rolling resistance coefficient
  const auto & hcog = get_base_config().chassis_config->cg_height;  // center of gravity height
  const auto & kroll_f = get_config().kroll_f;  // front roll moment distribution
  const auto & cl_f = get_base_config().aero_config->cl_f;  // downforce coefficient at front
  const auto & cl_r = get_base_config().aero_config->cl_r;  // downforce coefficient at rear
  const auto & rho = get_base_config().aero_config->air_density;  // air density
  const auto & A = get_base_config().aero_config->frontal_area;  // frontal area
  const auto & cd = get_base_config().aero_config->drag_coeff;  // drag coefficient
  const auto & mu = get_config().mu;  // tyre - track friction coefficient

  // magic tyre parameters
  const auto & tyre_f = *get_base_config().front_tyre_config;
  const auto & Bf = tyre_f.pacejka_b;  // magic formula B - front
  const auto & Cf = tyre_f.pacejka_c;  // magic formula C - front
  const auto & Ef = tyre_f.pacejka_e;  // magic formula E - front
  const auto & Fz0_f = tyre_f.pacejka_fz0;  // magic formula Fz0 - front
  const auto & eps_f = tyre_f.pacejka_eps;  // extended magic formula epsilon - front
  const auto & tyre_r = *get_base_config().rear_tyre_config;
  const auto & Br = tyre_r.pacejka_b;  // magic formula B - rear
  const auto & Cr = tyre_r.pacejka_c;  // magic formula C - rear
  const auto & Er = tyre_r.pacejka_e;  // magic formula E - rear
  const auto & Fz0_r = tyre_r.pacejka_fz0;  // magic formula Fz0 - rear
  const auto & eps_r = tyre_r.pacejka_eps;  // extended magic formula epsilon - rear

  // longitudinal tyre force Fx (eq. 4a, 4b)
  // TODO(haoru): consider differential
  const auto Fx_f = 0.5 * kd_f * fd + 0.5 * kb_f * fb - 0.5 * fr * m * GRAVITY * lr / l;
  const auto Fx_fl = Fx_f;
  const auto Fx_fr = Fx_f;
  const auto Fx_r = 0.5 * (1 - kd_f) * fd + 0.5 * (1.0 - kb_f) * fb - 0.5 * fr * m * GRAVITY * lf /
    l;
  const auto Fx_rl = Fx_r;
  const auto Fx_rr = Fx_r;

  // longitudinal acceleration (eq. 9)
  const auto ax = (fd + fb - 0.5 * cd * A * v_sq - fr * m * GRAVITY) / m;

  // vertical tyre force Fz (eq. 7a, 7b)
  const auto Fz_f = 0.5 * m * GRAVITY * lr / (lf + lr) - 0.5 * hcog / (lf + lr) * m * ax + 0.25 *
    cl_f * rho * A * v_sq;
  const auto Fz_fl = Fz_f - kroll_f * gamma_y;
  const auto Fz_fr = Fz_f + kroll_f * gamma_y;
  const auto Fz_r = 0.5 * m * GRAVITY * lr / (lf + lr) + 0.5 * hcog / (lf + lr) * m * ax + 0.25 *
    cl_r * rho * A * v_sq;
  const auto Fz_rl = Fz_r - (1.0 - kroll_f) * gamma_y;
  const auto Fz_rr = Fz_r + (1.0 - kroll_f) * gamma_y;

  // tyre sideslip angles alpha (eq. 6a, 6b)
  const auto a_fl = delta -
    atan((lf * omega + v * sin(beta)) / (v * cos(beta) - 0.5 * twf * omega));
  const auto a_fr = delta -
    atan((lf * omega + v * sin(beta)) / (v * cos(beta) + 0.5 * twf * omega));
  const auto a_rl = atan((lr * omega - v * sin(beta)) / (v * cos(beta) - 0.5 * twr * omega));
  const auto a_rr = atan((lr * omega - v * sin(beta)) / (v * cos(beta) + 0.5 * twr * omega));

  // lateral tyre force Fy (eq. 5)
  const auto Fy_fl = mu * Fz_fl * (1.0 + eps_f * Fz_fl / Fz0_f) *
    sin(Cf * atan(Bf * a_fl - Ef * (Bf * a_fl - atan(Bf * a_fl))));
  const auto Fy_fr = mu * Fz_fr * (1.0 + eps_f * Fz_fr / Fz0_f) *
    sin(Cf * atan(Bf * a_fr - Ef * (Bf * a_fr - atan(Bf * a_fr))));
  const auto Fy_rl = mu * Fz_rl * (1.0 + eps_r * Fz_rl / Fz0_r) *
    sin(Cr * atan(Br * a_rl - Er * (Br * a_rl - atan(Br * a_rl))));
  const auto Fy_rr = mu * Fz_rr * (1.0 + eps_r * Fz_rr / Fz0_r) *
    sin(Cr * atan(Br * a_rr - Er * (Br * a_rr - atan(Br * a_rr))));

  // dynamics (eq. 3a, 3b, 3c)
  const auto v_dot = 1.0 / m *
    ((Fx_rl + Fx_rr) * cos(beta) + (Fx_fl + Fx_fr) * cos(delta - beta) + (Fy_rl + Fy_rr) *
    sin(beta) - (Fy_fl + Fy_fr) * sin(delta - beta) - 0.5 * cd * rho * A * v_sq * cos(beta));
  const auto beta_dot = -omega + 1.0 / (m * v) *
    (-(Fx_rl + Fx_rr) * sin(beta) + (Fx_fl + Fx_fr) * sin(delta - beta) + (Fy_rl + Fy_rr) *
    cos(beta) + (Fy_fl + Fy_fr) * cos(delta - beta) + 0.5 * cd * rho * A * v_sq * sin(beta));
  const auto omega_dot = 1.0 / Jzz *
    ((Fx_rr - Fx_rl) * twr / 2 - (Fy_rl + Fy_rr) * lr +
    ((Fx_fr - Fx_fl) * cos(delta) + (Fy_fl - Fy_fr) * sin(delta)) * twf / 2.0 +
    ((Fy_fl + Fy_fr) * cos(delta) + (Fx_fl + Fx_fr) * sin(delta)) * lf);

  // cg position
  const auto vx = v * cos(phi);
  const auto vy = v * sin(phi);

  const auto x_dot = vertcat(vx, vy, omega, omega_dot, beta_dot, v_dot);
  const auto Fx_ij = vertcat(Fx_fl, Fx_fr, Fx_rl, Fx_rr);
  const auto Fy_ij = vertcat(Fy_fl, Fy_fr, Fy_rl, Fy_rr);
  const auto Fz_ij = vertcat(Fz_fl, Fz_fr, Fz_rl, Fz_rr);

  dynamics_ = casadi::Function(
    "double_track_planar_model",
    {x, u, gamma_y},
    {x_dot, Fx_ij, Fy_ij, Fz_ij},
    {"x", "u", "gamma_y"},
    {"x_dot", "Fx_ij", "Fy_ij", "Fz_ij"}
  );

  // const auto res = gamma_y - hcog / (0.5 * (twf + twr)) *
  //   (Fy_rl + Fy_rr + (Fx_rl + Fx_fr) * sin(delta) + (Fy_fl + Fy_fr) * cos(delta));
  // auto g = casadi::Function("g", {gamma_y}, {res});
  // lateral_load_transfer_ = casadi::rootfinder("G", "newton", g, {{"error_on_fail", false}});
}
}  // namespace double_track_planar_model
}  // namespace vehicle_model
}  // namespace lmpc