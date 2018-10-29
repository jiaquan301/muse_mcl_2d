#include "beam_model_amcl_normalized.h"


#include <cslibs_plugins_data/types/laserscan.hpp>

#include <muse_mcl_2d_gridmaps/maps/binary_gridmap.h>

#include <class_loader/class_loader_register_macro.h>
CLASS_LOADER_REGISTER_CLASS(muse_mcl_2d_gridmaps::BeamModelAMCLNormalized, muse_mcl_2d::UpdateModel2D)

namespace muse_mcl_2d_gridmaps {
BeamModelAMCLNormalized::BeamModelAMCLNormalized()
{
}

void BeamModelAMCLNormalized::apply(const data_t::ConstPtr &data,
                          const state_space_t::ConstPtr    &map,
                          sample_set_t::weight_iterator_t   set)
{
    if(!map->isType<BinaryGridmap>()) {
        return;
    }

    const cslibs_gridmaps::static_maps::BinaryGridmap &gridmap    = *(map->as<BinaryGridmap>().data());
    const cslibs_plugins_data::types::Laserscan              &laser_data = data->as<cslibs_plugins_data::types::Laserscan>();
    const cslibs_plugins_data::types::Laserscan::rays_t      &laser_rays = laser_data.getRays();

    /// laser to base transform
    cslibs_math_2d::Transform2d b_T_l;
    cslibs_math_2d::Transform2d m_T_w;
    if(!tf_->lookupTransform(robot_base_frame_,
                             laser_data.getFrame(),
                             ros::Time(laser_data.getTimeFrame().end.seconds()),
                             b_T_l,
                             tf_timeout_))
        return;
    if(!tf_->lookupTransform(world_frame_,
                             map->getFrame(),
                             ros::Time(laser_data.getTimeFrame().end.seconds()),
                             m_T_w,
                             tf_timeout_))
        return;

    const cslibs_plugins_data::types::Laserscan::rays_t rays = laser_data.getRays();
    const auto end = set.end();
    const auto const_end = set.const_end();
    const std::size_t rays_size = rays.size();
    const std::size_t ray_step  = std::max(1ul, (rays_size - 1) / (max_beams_ - 1));
    const double range_max = laser_data.getLinearMax();
    const double p_rand = z_rand_ * 1.0 / range_max;

    if(ps_.size() != set.capacity()) {
        ps_.resize(set.capacity(), 0.0);
    }
    std::fill(ps_.begin(), ps_.end(), 0.0);

    /// mixture distribution entries
    auto pow2 = [](const double x) {return x*x;};
    auto pow3   = [](const double x) {return x*x*x;};
    auto p_hit = [this, &pow2](const double ray_range, const double map_range) {
        return z_hit_ * std::exp(pow2(ray_range - map_range) * denominator_exponent_hit_);
    };
    auto p_short = [this](const double ray_range, const double map_range) {
        return ray_range < map_range ? z_short_ * lambda_short_ * std::exp(-lambda_short_ * ray_range) : 0.0;
        //return ray_range < map_range ? z_short_ * /*(1.0 / (1.0 - std::exp(-lambda_short_  * map_range))) **/ lambda_short_ * std::exp(-lambda_short_ * ray_range)
        //                               : 0.0;
    };
    auto p_max = [this, &range_max](const double ray_range) {
        return ray_range >= range_max ? z_max_ : 0.0;
    };
    auto p_random = [this, &range_max, &p_rand](const double ray_range) {
        return ray_range < range_max ? p_rand : 0.0;
    };
    auto probability = [&gridmap, &p_hit, &p_short, &p_max, &p_random, &range_max]
            (const cslibs_plugins_data::types::Laserscan::Ray &ray, const cslibs_math_2d::Pose2d &m_T_l) {
        const double ray_range = ray.range;
        auto         ray_end_point = m_T_l * ray.end_point;
        const double map_range = std::min(range_max, gridmap.getRange(m_T_l.translation(), ray_end_point));
        return p_hit(ray_range, map_range) + p_short(ray_range, map_range) + p_max(ray_range) + p_random(ray_range);
    };

    auto it_ps = ps_.begin();
    double max = std::numeric_limits<double>::lowest();
    for(auto it = set.const_begin() ; it != const_end ; ++it, ++it_ps) {
        const cslibs_math_2d::Pose2d m_T_l = m_T_w * it->state * b_T_l; /// laser scanner pose in map coordinates
        double p = 1.0;
        for(std::size_t i = 0 ; i < rays_size ;  i+= ray_step) {
            const auto &ray = laser_rays[i];
            p += ray.valid() ? pow3(probability(ray, m_T_l)) : pow3(z_max_);
        }
        *it_ps = p;
        max = p >= max ? p : max;
    }

    it_ps = ps_.begin();
    for(auto it = set.begin() ; it != end ; ++it, ++it_ps) {
        *it *= *it_ps / max;
    }

}

void BeamModelAMCLNormalized::doSetup(ros::NodeHandle &nh)
{
    auto param_name = [this](const std::string &name){return name_ + "/" + name;};

    max_beams_    = nh.param(param_name("max_beams"), 30);
    z_hit_        = nh.param(param_name("z_hit"), 0.8);
    z_short_      = nh.param(param_name("z_short"), 0.1);
    z_max_        = nh.param(param_name("z_max"), 0.05);
    z_rand_       = nh.param(param_name("z_rand"), 0.05);
    sigma_hit_    = nh.param(param_name("sigma_hit"), 0.15);
    denominator_exponent_hit_ = -0.5 * 1.0 / (sigma_hit_ * sigma_hit_);
    lambda_short_ = nh.param(param_name("lambda_short"), 0.01);
}
}
