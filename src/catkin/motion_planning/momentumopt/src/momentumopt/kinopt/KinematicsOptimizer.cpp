#include <iomanip>
#include <fstream>
#include <yaml-cpp/yaml.h>
//#include <momentumopt/utilities/OrientationUtils.hpp>
#include <momentumopt/kinopt/KinematicsOptimizer.hpp>

using namespace solver;

namespace momentumopt {

  void KinematicsOptimizer::initialize(PlannerSetting& planner_setting, KinematicsInterface* kin_interface)
  {
    kin_interface_ = kin_interface;
    planner_setting_ = &planner_setting;
    kin_interface_->internalInitialization(planner_setting);

    current_state_ = new KinematicsState(this->getSetting().get(PlannerIntParam_NumDofs));

    kinematicsSequence().resize(this->getSetting().get(PlannerIntParam_NumTimesteps), this->getSetting().get(PlannerIntParam_NumDofs));
    if (!this->getSetting().get(PlannerBoolParam_UseDefaultSolverSetting)) { model_.configSetting(this->getSetting().get(PlannerStringParam_ConfigFile)); }
    else                                                                   { model_.configSetting(this->getSetting().get(PlannerStringParam_DefaultSolverSettingFile)); }

    //nonlinear_terms_.resize(this->getSetting().get(PlannerIntParam_NumDofs)+6);
    //base_jacobian_.resize(6,this->getSetting().get(PlannerIntParam_NumDofs)+6);
    //inertia_matrix_.resize(this->getSetting().get(PlannerIntParam_NumDofs)+6, this->getSetting().get(PlannerIntParam_NumDofs)+6);

    this->initializeKinematicVariables();
    //eef_rot_kin_optimizer_.initialize(kin_interface_);
    //kin_interface_->initialize(this->getSetting().get(PlannerIntVectorParam_RotationDofsForEndeffector));
  }

  void KinematicsOptimizer::initializeKinematicVariables()
  {
    num_vars_ = 0.;
    double inf_value = SolverSetting::inf;

    // center of mass, linear and angular momentum
    com_.initialize('C', 3, 1, -inf_value, inf_value, num_vars_);
    comd_.initialize('C', 3, 1, -inf_value, inf_value, num_vars_);
    lmom_.initialize('C', 3, 1, -inf_value, inf_value, num_vars_);
    amom_.initialize('C', 3, 1, -inf_value, inf_value, num_vars_);

    // end-effector positions and velocities
    for (int eff_id=0; eff_id<Problem::n_endeffs_; eff_id++) {
      eef_pos_[eff_id].initialize('C', 3, 1, -inf_value, inf_value, num_vars_);
      eef_vel_[eff_id].initialize('C', 3, 1, -inf_value, inf_value, num_vars_);
    }

    // joint positions, velocities and accelerations
    jnt_q_.initialize('C', this->getSetting().get(PlannerIntParam_NumActiveDofs), 1, -inf_value, inf_value, num_vars_);
    jnt_qd_.initialize('C', this->getSetting().get(PlannerIntParam_NumActiveDofs), 1, -inf_value, inf_value, num_vars_);
    total_qd_.initialize('C', this->getSetting().get(PlannerIntParam_NumActiveDofs), 1, -inf_value, inf_value, num_vars_);
    total_qdd_.initialize('C', this->getSetting().get(PlannerIntParam_NumActiveDofs), 1, -inf_value, inf_value, num_vars_);
  }

  void KinematicsOptimizer::optimizePosture(KinematicsState& current_state, const DynamicsState& desired_state, bool is_not_initial_state, bool include_torque_limits)
  {
    KinematicsState start_state = current_state;
    double cur_error, prev_error = SolverSetting::inf;

	for (int iter_id=0; iter_id<this->getSetting().get(PlannerIntParam_MaxKinConvergenceIterations); iter_id++) {
      current_state = kin_interface_->updateJacobians(current_state);

      // computing error norm and convergence check
      cur_error = 0.0;
      for (int axis_id=0; axis_id<0; axis_id++) {
        cur_error += this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingCenterOfMass)[axis_id]*
                     std::pow(current_state.centerOfMass()[axis_id]-desired_state.centerOfMass()[axis_id], 2.0);
        for (int eff_id=0; eff_id<this->getSetting().get(PlannerIntParam_NumActiveEndeffectors); eff_id++) {
          cur_error += this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingEndeffectorPosition)[axis_id]*
                       std::pow(current_state.endeffectorPosition(eff_id)[axis_id]-desired_state.endeffectorPosition(eff_id)[axis_id], 2.0);
        }
      }

      if (cur_error<this->getSetting().get(PlannerDoubleParam_KinConvergenceTolerance) ||
          std::abs(prev_error-cur_error)<this->getSetting().get(PlannerDoubleParam_KinConvergenceTolerance))
      { break; }
      prev_error = cur_error;

	  try
      {
        // add variables to model
        vars_.clear();
        for (int var_id=0; var_id<num_vars_; var_id++)
          vars_.push_back(Var());

        model_.clean();
        addVariableToModel(com_, model_, vars_);
        addVariableToModel(comd_, model_, vars_);
        addVariableToModel(lmom_, model_, vars_);
        addVariableToModel(amom_, model_, vars_);

        addVariableToModel(jnt_q_, model_, vars_);
        addVariableToModel(jnt_qd_, model_, vars_);
        addVariableToModel(total_qd_, model_, vars_);
        addVariableToModel(total_qdd_, model_, vars_);

        for (int eff_id=0; eff_id<Problem::n_endeffs_; eff_id++) {
          addVariableToModel(eef_pos_[eff_id], model_, vars_);
          addVariableToModel(eef_vel_[eff_id], model_, vars_);
        }

        // PROBLEM OBJECTIVE
        DCPQuadExpr quadobj;
        for (int axis_id=0; axis_id<3; axis_id++) {

          // penalty on tracking of center of mass, linear and angular momenta
          quadobj.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingCenterOfMass)[axis_id],
                             LinExpr(desired_state.centerOfMass()[axis_id]) - vars_[com_.id(axis_id,0)]);
          if (is_not_initial_state) {
            quadobj.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingLinearMomentum)[axis_id],
                               LinExpr(desired_state.linearMomentum()[axis_id]) - vars_[lmom_.id(axis_id,0)]);
            quadobj.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingAngularMomentum)[axis_id],
                               LinExpr(desired_state.angularMomentum()[axis_id]) - vars_[amom_.id(axis_id,0)]);
            quadobj.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingLinearMomentumRate)[axis_id],
                               LinExpr(vars_[lmom_.id(axis_id,0)], 1.0/desired_state.time())-LinExpr(start_state.linearMomentum()[axis_id]/desired_state.time()) );
            quadobj.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingAngularMomentumRate)[axis_id],
                               LinExpr(vars_[amom_.id(axis_id,0)], 1.0/desired_state.time())-LinExpr(start_state.angularMomentum()[axis_id]/desired_state.time()) );
          }

          //// penalty for tracking a desired base orientation
          //Eigen::Vector3d base_angular_velocity = requiredAngularVelocity(desired_state.baseOrientation(), current_state.baseOrientation(), desired_state.time(), this->getSetting().get(PlannerBoolParam_AngularVelocityInBodyCoordinates));
          //quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingBaseOrientation)[axis_id], base_angular_velocity[axis_id] - vars_[base_ang_vel_.id(axis_id,0)]);

          // penalty for tracking desired endeffector positions and orientations
          for (int eff_id=0; eff_id<this->getSetting().get(PlannerIntParam_NumActiveEndeffectors); eff_id++)
            if (!desired_state.endeffectorActivation(eff_id)) {
              //double weight_scale = desired_state.endeffectorActivationWeight(eff_id);
              //Eigen::Vector3d eff_angular_velocity = requiredAngularVelocity(desired_state.endeffectorOrientation(eff_id), current_state.endeffectorOrientation(eff_id), desired_state.time(), this->getSetting().get(PlannerBoolParam_AngularVelocityInBodyCoordinates));
              ////quadexpr.addQuaTerm(weight_scale*this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingEndeffectorOrientation)[axis_id], eff_angular_velocity[axis_id] - vars_[eef_ang_vel_[eff_id].id(axis_id,0)]);
              quadobj.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingEndeffectorPosition)[axis_id],
                                 LinExpr(desired_state.endeffectorPosition(eff_id)[axis_id]) - vars_[eef_pos_[eff_id].id(axis_id,0)]);
            }
        }

        // penalty on joint default posture
        for (int dof_id=6; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++)
          quadobj.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicDefaultJointPositions)[dof_id-6],
                             LinExpr(this->getSetting().get(PlannerVectorParam_KinematicDefaultJointPositions)[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]]-6) - vars_[jnt_q_.id(dof_id,0)]);

        // penalty on joint velocities
        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++)
          quadobj.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightJointVelocity)[dof_id], vars_[total_qd_.id(dof_id,0)]);

        // penalty on joint accelerations
        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++)
          quadobj.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightJointAcceleration)[dof_id], vars_[total_qdd_.id(dof_id,0)]);

        model_.setObjective(quadobj, 0.0);

        // PROBLEM CONSTRAINTS
        for (int axis_id=0; axis_id<3; axis_id++) {
          // center of mass, linear and angular momenta
          lin_cons_ = LinExpr(vars_[com_.id(axis_id,0)]) - LinExpr(current_state.centerOfMass()[axis_id]) - LinExpr(vars_[comd_.id(axis_id,0)], desired_state.time()/this->getSetting().get(PlannerDoubleParam_RobotMass));
          model_.addLinConstr(lin_cons_, "=",  0.0);

          lin_cons_ = LinExpr(vars_[comd_.id(axis_id,0)], -1.0);
          for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs)+6; dof_id++) {
            if (dof_id<6) { lin_cons_ += LinExpr(vars_[jnt_qd_.id(dof_id,0)], kin_interface_->centroidalMomentumMatrix()(axis_id, dof_id)); }
            else          { lin_cons_ += LinExpr(vars_[jnt_qd_.id(dof_id,0)], kin_interface_->centroidalMomentumMatrix()(axis_id, 6+this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id])); }
          }
          model_.addLinConstr(lin_cons_, "=",  0.0);

          lin_cons_ = LinExpr(vars_[lmom_.id(axis_id,0)], -1.0);
          for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs)+6; dof_id++)
            if (dof_id<6) { lin_cons_ += LinExpr(vars_[total_qd_.id(dof_id,0)], kin_interface_->centroidalMomentumMatrix()(axis_id, dof_id)); }
            else          { lin_cons_ += LinExpr(vars_[total_qd_.id(dof_id,0)], kin_interface_->centroidalMomentumMatrix()(axis_id, 6+this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id])); }
          model_.addLinConstr(lin_cons_, "=",  0.0);

          lin_cons_ = LinExpr(vars_[amom_.id(axis_id,0)], -1.0);
          for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs)+6; dof_id++)
            if (dof_id<6) { lin_cons_ += LinExpr(vars_[total_qd_.id(dof_id,0)], kin_interface_->centroidalMomentumMatrix()(3+axis_id, dof_id)); }
            else          { lin_cons_ += LinExpr(vars_[total_qd_.id(dof_id,0)], kin_interface_->centroidalMomentumMatrix()(3+axis_id, 6+this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id])); }
          model_.addLinConstr(lin_cons_, "=",  0.0);

          // end-effector positions, velocities and angular velocities
          for (int eff_id=0; eff_id<Problem::n_endeffs_; eff_id++) {
            lin_cons_ = LinExpr(vars_[eef_pos_[eff_id].id(axis_id,0)]) - LinExpr(current_state.endeffectorPosition(eff_id)[axis_id]) - LinExpr(vars_[eef_vel_[eff_id].id(axis_id,0)], desired_state.time());
            model_.addLinConstr(lin_cons_, "=",  0.0);

            lin_cons_ = LinExpr(vars_[eef_vel_[eff_id].id(axis_id,0)], -1.0);
            for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs)+6; dof_id++)
              if (dof_id<6) { lin_cons_ += LinExpr(vars_[jnt_qd_.id(dof_id,0)], kin_interface_->endeffectorJacobian(eff_id)(axis_id, dof_id)); }
              else          { lin_cons_ += LinExpr(vars_[jnt_qd_.id(dof_id,0)], kin_interface_->endeffectorJacobian(eff_id)(axis_id, 6+this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id])); }
            model_.addLinConstr(lin_cons_, "=",  0.0);

            if (desired_state.endeffectorActivation(eff_id)) {
              //if (is_not_initial_state) { model_.addLinConstr(vars_[eef_ang_vel_[eff_id].id(axis_id,0)], "=", 0.0); }
              model_.addLinConstr(LinExpr(desired_state.endeffectorPosition(eff_id)[axis_id]) - LinExpr(vars_[eef_pos_[eff_id].id(axis_id,0)]), "=",  0.0);
            }
          }
        }

        // joint positions
        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++) {
          lin_cons_ = LinExpr(vars_[jnt_q_.id(dof_id,0)]) - (LinExpr(current_state.robotPosture().jointPositions()(this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]))+LinExpr(vars_[jnt_qd_.id(dof_id+6,0)],desired_state.time()));
          model_.addLinConstr(lin_cons_, "=",  0.0);
        }

        // joint velocities
        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs)+6; dof_id++) {
          if (dof_id>0 && dof_id<3) { // Base linear velocity
            //lin_cons_ = LinExpr(vars_[total_qd_.id(dof_id,0)]) - (LinExpr(vars_[base_position_.id(dof_id,0)], 1.0/desired_state.time()) - LinExpr(start_state.robotPosture().basePosition()[dof_id]/desired_state.time()));
          } else if (dof_id>3 && dof_id<6) { // Base angular velocity
            // ToDo: properly compute total angular velocity
          } else { // joint velocities
            lin_cons_ = LinExpr(vars_[total_qd_.id(dof_id,0)]) - (LinExpr(vars_[jnt_q_.id(dof_id,0)], 1.0/desired_state.time()));
          }
          //lin_cons_ = vars_[total_qd_.id(dof_id,0)] - (vars_[jnt_q_.id(dof_id-6,0)] - start_state.robotPosture().jointPositions()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id-6]])/desired_state.time();
          model_.addLinConstr(lin_cons_, "=",  0.0);
        }

        // joint accelerations
        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs)+6; dof_id++) {
          if (dof_id<6) { lin_cons_ = LinExpr(vars_[total_qdd_.id(dof_id,0)]) - (LinExpr(vars_[total_qd_.id(dof_id,0)], 1.0/desired_state.time()) - LinExpr(start_state.robotVelocity().generalizedJointVelocities()[dof_id]/desired_state.time())); }
          else          { lin_cons_ = LinExpr(vars_[total_qdd_.id(dof_id,0)]) - (LinExpr(vars_[total_qd_.id(dof_id,0)], 1.0/desired_state.time()) - LinExpr(start_state.robotVelocity().jointVelocities()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id-6]]/desired_state.time())); }
          model_.addLinConstr(lin_cons_, "=",  0.0);
        }

        // joint limits
        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++) {
          //if (this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id] < this->getSetting().get(PlannerIntParam_NumDofs)) {
          model_.addLinConstr(LinExpr(vars_[jnt_q_.id(dof_id,0)]), ">", LinExpr(this->getSetting().get(PlannerVectorParam_MinJointAngles)[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]]));
          model_.addLinConstr(LinExpr(vars_[jnt_q_.id(dof_id,0)]), "<", LinExpr(this->getSetting().get(PlannerVectorParam_MaxJointAngles)[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]]));
          //}
        }

        // Write problem in standard conic form and solve it
        exitcode_ = model_.optimize();

        solution_.resize(num_vars_, 1);  solution_.setZero();
        for (int var_id=0; var_id<num_vars_; var_id++)
          solution_(var_id) = vars_[var_id].get(SolverDoubleParam_X);

        saveSolution(com_);
        saveSolution(comd_);
        saveSolution(lmom_);
        saveSolution(amom_);
        saveSolution(jnt_q_);
        saveSolution(jnt_qd_);
        saveSolution(total_qd_);
        saveSolution(total_qdd_);
        for (int eff_id=0; eff_id<Problem::n_endeffs_; eff_id++) {
          saveSolution(eef_pos_[eff_id]);
          saveSolution(eef_vel_[eff_id]);
        }

        // storing results
        lmom_.getGuessValue(mat_guess_);   current_state.linearMomentum() = mat_guess_.col(0);
        amom_.getGuessValue(mat_guess_);   current_state.angularMomentum() = mat_guess_.col(0);

      }
      catch(...)
      {
        std::cout << "Exception during posture-generation optimization" << std::endl;
      }

//      // computing resulting state
//      Eigen::Vector3d base_angular_velocity = Eigen::Vector3d(vars_[base_ang_vel_.id(0,0)].get(SolverDoubleParam_X),
//                                                              vars_[base_ang_vel_.id(1,0)].get(SolverDoubleParam_X),
//                                                              vars_[base_ang_vel_.id(2,0)].get(SolverDoubleParam_X));
//      current_state_.baseOrientation() = integrateAngularVelocity(current_state_.baseOrientation(), base_angular_velocity, desired_state.time(), this->getSetting().get(PlannerBoolParam_AngularVelocityInBodyCoordinates));
//      for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++) {
//        current_state.jointPositions()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]] = vars_[jnt_q_.id(dof_id,0)].get(SolverDoubleParam_X);
//        current_state.jointVelocities()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]] = vars_[total_qd_.id(dof_id,0)].get(SolverDoubleParam_X);
//        current_state.jointAccelerations()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]] = vars_[total_qdd_.id(dof_id,0)].get(SolverDoubleParam_X);
//      }




//	  kin_interface_->updateJacobianAndState(centroidal_momentum_matrix_, base_jacobian_, endeffector_jacobian_, current_state);
//
//      // computing error norm
//	  Eigen::Vector3d base_orientation_error = orientationError(desired_state.baseOrientation(), current_state.baseOrientation());
//      cur_error = (base_orientation_error).dot(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingBaseOrientation).asDiagonal()*base_orientation_error);
//	  cur_error += (current_state.centerOfMass()-desired_state.centerOfMass()).dot(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingCenterOfMass).asDiagonal()*(current_state.centerOfMass()-desired_state.centerOfMass()));
//      for (int eff_id=0; eff_id<this->getSetting().get(PlannerIntParam_NumActiveEndeffectors); eff_id++) {
//        double weight_scale = desired_state.endeffectorActivationWeight(eff_id);
//        Eigen::Vector3d eff_orientation_error = orientationError(desired_state.endeffectorOrientation(eff_id), current_state.endeffectorOrientation(eff_id));
//        cur_error += (eff_orientation_error).dot(weight_scale*this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingEndeffectorOrientation).asDiagonal()*eff_orientation_error);
//        cur_error += (current_state.endeffectorPosition(eff_id)-desired_state.endeffectorPosition(eff_id)).dot(weight_scale*this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingEndeffectorPosition).asDiagonal()*(current_state.endeffectorPosition(eff_id)-desired_state.endeffectorPosition(eff_id)));
//      }
//
//      if (cur_error<this->getSetting().get(PlannerDoubleParam_KinematicConvergenceTolerance) ||
//          std::abs(prev_error-cur_error)<this->getSetting().get(PlannerDoubleParam_KinematicConvergenceTolerance))
//      { break; }
//      prev_error = cur_error;
//
//	  try
//      {
//        // add variables to model
//        vars_.clear();
//        for (int var_id=0; var_id<num_vars_; var_id++)
//    	      vars_.push_back(Var());
//
//        model_.clean();
//        addVariableToModel(com_, model_, vars_);
//        addVariableToModel(comd_, model_, vars_);
//        addVariableToModel(lmom_, model_, vars_);
//        addVariableToModel(amom_, model_, vars_);
//        addVariableToModel(jnt_q_, model_, vars_);
//        addVariableToModel(jnt_qd_, model_, vars_);
//        addVariableToModel(total_qd_, model_, vars_);
//        addVariableToModel(total_qdd_, model_, vars_);
//        addVariableToModel(base_ang_vel_, model_, vars_);
//        for (int eff_id=0; eff_id<Problem::n_endeffs_; eff_id++) {
//          addVariableToModel(eef_pos_[eff_id], model_, vars_);
//          addVariableToModel(eef_vel_[eff_id], model_, vars_);
//          //addVariableToModel(eef_ang_vel_[eff_id], model_, vars_);
//        }
//
//        //  PROBLEM OBJECTIVE
//        DCPQuadExpr quadexpr;
//        for (int axis_id=0; axis_id<3; axis_id++) {
//
//          // penalty on tracking of center of mass, linear and angular momenta
//          quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingCenterOfMass)[axis_id], desired_state.centerOfMass()[axis_id] - vars_[com_.id(axis_id,0)]);
//          if (is_not_initial_state) {
//            quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingLinearMomentum)[axis_id], desired_state.linearMomentum()[axis_id] - vars_[lmom_.id(axis_id,0)]);
//            quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingAngularMomentum)[axis_id], desired_state.angularMomentum()[axis_id] - vars_[amom_.id(axis_id,0)]);
//            quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingLinearMomentumRate)[axis_id], (lmom_.id(axis_id,0)-start_state.linearMomentum()[axis_id])/desired_state.time() );
//            quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingAngularMomentumRate)[axis_id], (amom_.id(axis_id,0)-start_state.angularMomentum()[axis_id])/desired_state.time() );
//          }
//
//          // penalty for tracking a desired base orientation
//          Eigen::Vector3d base_angular_velocity = requiredAngularVelocity(desired_state.baseOrientation(), current_state.baseOrientation(), desired_state.time(), this->getSetting().get(PlannerBoolParam_AngularVelocityInBodyCoordinates));
//          quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingBaseOrientation)[axis_id], base_angular_velocity[axis_id] - vars_[base_ang_vel_.id(axis_id,0)]);
//
//          // penalty for tracking desired endeffector positions and orientations
//          for (int eff_id=0; eff_id<this->getSetting().get(PlannerIntParam_NumActiveEndeffectors); eff_id++)
//            if (!desired_state.endeffectorActivation(eff_id)) {
//              double weight_scale = desired_state.endeffectorActivationWeight(eff_id);
//              Eigen::Vector3d eff_angular_velocity = requiredAngularVelocity(desired_state.endeffectorOrientation(eff_id), current_state.endeffectorOrientation(eff_id), desired_state.time(), this->getSetting().get(PlannerBoolParam_AngularVelocityInBodyCoordinates));
//              //quadexpr.addQuaTerm(weight_scale*this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingEndeffectorOrientation)[axis_id], eff_angular_velocity[axis_id] - vars_[eef_ang_vel_[eff_id].id(axis_id,0)]);
//              quadexpr.addQuaTerm(weight_scale*this->getSetting().get(PlannerVectorParam_WeightKinematicTrackingEndeffectorPosition)[axis_id], desired_state.endeffectorPosition(eff_id)[axis_id] - vars_[eef_pos_[eff_id].id(axis_id,0)]);
//            }
//        }
//
//        // penalty on joint default posture
//        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++)
//          if (this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id] < this->getSetting().get(PlannerIntParam_NumDofs))
//            quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightKinematicDefaultJointState)[dof_id], this->getSetting().get(PlannerVectorParam_DefaultJointState)[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]] - vars_[jnt_q_.id(dof_id,0)]);
//
//        // penalty on joint velocities
//        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++)
//          quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightJointVelocity)[dof_id], vars_[total_qd_.id(dof_id,0)]);
//
//        // penalty on joint accelerations
//        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++)
//          quadexpr.addQuaTerm(this->getSetting().get(PlannerVectorParam_WeightJointAcceleration)[dof_id], vars_[total_qdd_.id(dof_id,0)]);
//
//        model_.setObjective(quadexpr, 0.0);
//
//        // PROBLEM CONSTRAINTS
//        for (int axis_id=0; axis_id<3; axis_id++) {
//          // center of mass, linear and angular momenta
//          lin_cons_ = vars_[com_.id(axis_id,0)] - current_state.centerOfMass()[axis_id] - desired_state.time()/this->getSetting().get(PlannerDoubleParam_RobotMass)*vars_[comd_.id(axis_id,0)];
//          model_.addLinConstr(lin_cons_, "=",  0.0);
//
//          lin_cons_ = -vars_[comd_.id(axis_id,0)];
//          for (int dof=0; dof<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof++)
//            lin_cons_ += centroidal_momentum_matrix_(axis_id,this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof])*vars_[jnt_qd_.id(dof,0)];
//          model_.addLinConstr(lin_cons_, "=",  0.0);
//
//          lin_cons_ = -vars_[lmom_.id(axis_id,0)];
//          for (int dof=0; dof<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof++)
//            lin_cons_ += centroidal_momentum_matrix_(axis_id,this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof])*vars_[total_qd_.id(dof,0)];
//          model_.addLinConstr(lin_cons_, "=",  0.0);
//
//          lin_cons_ = -vars_[amom_.id(axis_id,0)];
//          for (int dof=0; dof<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof++)
//        	    lin_cons_ += centroidal_momentum_matrix_(3+axis_id,this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof])*vars_[total_qd_.id(dof,0)];
//          model_.addLinConstr(lin_cons_, "=",  0.0);
//
//          // end-effector positions, velocities and angular velocities
//          for (int eff_id=0; eff_id<Problem::n_endeffs_; eff_id++) {
//            lin_cons_ = vars_[eef_pos_[eff_id].id(axis_id,0)] - current_state.endeffectorPosition(eff_id)[axis_id] - desired_state.time()*vars_[eef_vel_[eff_id].id(axis_id,0)];
//            model_.addLinConstr(lin_cons_, "=",  0.0);
//
//            lin_cons_ = -vars_[eef_vel_[eff_id].id(axis_id,0)];
//            for (int dof=0; dof<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof++)
//              lin_cons_ += endeffector_jacobian_[eff_id](axis_id,this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof])*vars_[jnt_qd_.id(dof,0)];
//            model_.addLinConstr(lin_cons_, "=",  0.0);
//
//            //lin_cons_ = -vars_[eef_ang_vel_[eff_id].id(axis_id,0)];
//            //for (int dof=0; dof<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof++)
//            //  lin_cons_ += endeffector_jacobian_[eff_id](3+axis_id,this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof])*vars_[jnt_qd_.id(dof,0)];
//            //model_.addLinConstr(lin_cons_, "=",  0.0);
//
//            if (desired_state.endeffectorActivation(eff_id)) {
//              //if (is_not_initial_state) { model_.addLinConstr(vars_[eef_ang_vel_[eff_id].id(axis_id,0)], "=", 0.0); }
//              model_.addLinConstr(desired_state.endeffectorPosition(eff_id)[axis_id] - vars_[eef_pos_[eff_id].id(axis_id,0)], "=",  0.0);
//            }
//          }
//
//          // base angular velocity for base orientation tracking
//          lin_cons_ = -vars_[base_ang_vel_.id(axis_id,0)];
//          for (int dof=0; dof<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof++)
//            lin_cons_ += base_jacobian_(3+axis_id,this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof])*vars_[jnt_qd_.id(dof,0)];
//          model_.addLinConstr(lin_cons_, "=",  0.0);
//        }
//
//        // joint positions
//        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++) {
//          lin_cons_ = vars_[jnt_q_.id(dof_id,0)] - (current_state.jointPositions()(this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id])+desired_state.time()*vars_[jnt_qd_.id(dof_id,0)]);
//          model_.addLinConstr(lin_cons_, "=",  0.0);
//        }
//
//        // joint velocities
//        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++) {
//          lin_cons_ = vars_[total_qd_.id(dof_id,0)] - (vars_[jnt_q_.id(dof_id,0)] - start_state.jointPositions()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]])/desired_state.time();
//          model_.addLinConstr(lin_cons_, "=",  0.0);
//        }
//
//        // joint accelerations
//        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++) {
//          lin_cons_ = vars_[total_qdd_.id(dof_id,0)] - (vars_[total_qd_.id(dof_id,0)] - start_state.jointVelocities()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]])/desired_state.time();
//          model_.addLinConstr(lin_cons_, "=",  0.0);
//        }
//
//        // joint limits
//        for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++) {
//          if (this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id] < this->getSetting().get(PlannerIntParam_NumDofs)) {
//            model_.addLinConstr(vars_[jnt_q_.id(dof_id,0)], ">", this->getSetting().get(PlannerVectorParam_MinJointAngles)[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]]);
//            model_.addLinConstr(vars_[jnt_q_.id(dof_id,0)], "<", this->getSetting().get(PlannerVectorParam_MaxJointAngles)[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]]);
//          }
//        }
//
////        // torque limits constraint
////        if (this->getSetting().get(PlannerBoolParam_HasTorqueLimits) && is_not_initial_state && include_torque_limits) {
////          kin_interface_->updateInertiaAndNonlinearTerms(inertia_matrix_, nonlinear_terms_, current_state);
////
////          for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++) {
////            int current_dof = this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id];
////            if (current_dof < this->getSetting().get(PlannerIntParam_NumDofs)) {
////              lin_cons_ = nonlinear_terms_[current_dof];
////              for (int col_id=0; col_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); col_id++)
////                lin_cons_ += inertia_matrix_(current_dof, this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[col_id])*vars_[total_qdd_.id(col_id,0)];
////
////              for (int eff_id=0; eff_id<this->getSetting().get(PlannerIntParam_NumActiveEndeffectors); eff_id++) {
////                Eigen::Matrix3d eff_rotation = desired_state.endeffectorOrientation(eff_id).toRotationMatrix();
////                if (desired_state.endeffectorActivation(eff_id)) {
////                  for (int axis_id=0; axis_id<3; axis_id++) {
////                    lin_cons_ += -endeffector_jacobian_[eff_id](axis_id,current_dof)*(this->getSetting().get(PlannerDoubleParam_MassTimesGravity)*desired_state.endeffectorForce(eff_id)[axis_id]);
////                    for (int row_id=0; row_id<3; row_id++)
////                      lin_cons_ += -endeffector_jacobian_[eff_id](3+axis_id,current_dof)*eff_rotation(axis_id, row_id)*desired_state.endeffectorTorqueAtContactPoint(eff_id)[row_id];
////                  }
////                }
////              }
////              model_.addLinConstr(lin_cons_, "<",  this->getSetting().get(PlannerVectorParam_MaxTorqueLimits)[current_dof]);
////              model_.addLinConstr(lin_cons_, ">",  this->getSetting().get(PlannerVectorParam_MinTorqueLimits)[current_dof]);
////            }
////          }
////        }
//
//        // Write problem in standard conic form and solve it
//        exitcode_ = model_.optimize();
//
//        solution_.resize(num_vars_, 1);  solution_.setZero();
//        for (int var_id=0; var_id<num_vars_; var_id++)
//          solution_(var_id) = vars_[var_id].get(SolverDoubleParam_X);
//
//        saveSolution(com_);
//        saveSolution(comd_);
//        saveSolution(lmom_);
//        saveSolution(amom_);
//        saveSolution(jnt_q_);
//        saveSolution(jnt_qd_);
//        saveSolution(total_qd_);
//        saveSolution(total_qdd_);
//        for (int eff_id=0; eff_id<Problem::n_endeffs_; eff_id++) {
//          saveSolution(eef_pos_[eff_id]);
//          saveSolution(eef_vel_[eff_id]);
//        }
//
//        // storing results
//        lmom_.getGuessValue(mat_guess_);   current_state.linearMomentum() = mat_guess_.col(0);
//        amom_.getGuessValue(mat_guess_);   current_state.angularMomentum() = mat_guess_.col(0);
//      }
//      catch(...)
//      {
//        std::cout << "Exception during posture-generation optimization" << std::endl;
//      }
//
//      // computing resulting state
//      Eigen::Vector3d base_angular_velocity = Eigen::Vector3d(vars_[base_ang_vel_.id(0,0)].get(SolverDoubleParam_X),
//                                                              vars_[base_ang_vel_.id(1,0)].get(SolverDoubleParam_X),
//                                                              vars_[base_ang_vel_.id(2,0)].get(SolverDoubleParam_X));
//      current_state_.baseOrientation() = integrateAngularVelocity(current_state_.baseOrientation(), base_angular_velocity, desired_state.time(), this->getSetting().get(PlannerBoolParam_AngularVelocityInBodyCoordinates));
//      for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntParam_NumActiveDofs); dof_id++) {
//        current_state.jointPositions()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]] = vars_[jnt_q_.id(dof_id,0)].get(SolverDoubleParam_X);
//        current_state.jointVelocities()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]] = vars_[total_qd_.id(dof_id,0)].get(SolverDoubleParam_X);
//        current_state.jointAccelerations()[this->getSetting().get(PlannerIntVectorParam_ActiveDofs)[dof_id]] = vars_[total_qdd_.id(dof_id,0)].get(SolverDoubleParam_X);
//      }
//
//      for (int eff_id=0; eff_id<this->getSetting().get(PlannerIntParam_NumActiveEndeffectors); eff_id++)
//        for (int axis_id=0; axis_id<3; axis_id++) {
//          current_state.endeffectorPosition(eff_id)[axis_id] = vars_[eef_pos_[eff_id].id(axis_id,0)].get(SolverDoubleParam_X);
//          current_state.endeffectorVelocity(eff_id)[axis_id] = vars_[eef_vel_[eff_id].id(axis_id,0)].get(SolverDoubleParam_X);
//        }
	}
//
//	// optimization of end-effector rotations with a non convex solver
//	if (this->getSetting().get(PlannerBoolParam_UseNoncvxEndeffectorOrientationOpt)) {
//	  kin_interface_->setDesiredEndeffectorOrientation(desired_state);
//      eef_rot_kin_optimizer_.optimize();
//      for (int dof_id=0; dof_id<this->getSetting().get(PlannerIntVectorParam_RotationDofsForEndeffector).size(); dof_id++)
//        current_state.jointPositions()[this->getSetting().get(PlannerIntVectorParam_RotationDofsForEndeffector)(dof_id)] = kin_interface_->optimalVector()[dof_id];
//	}
  }

  void KinematicsOptimizer::optimize(const DynamicsState& ini_state, const DynamicsSequence& dyn_sequence, bool is_not_first_kindyn_iteration)
  {
    std::cout << "BeforeOptimize" << std::endl;
    this->optimizePosture(*current_state_, ini_state, false, is_not_first_kindyn_iteration);
    std::cout << "AfterOptimize" << std::endl;

//    ini_state_ = ini_state;
//    if (this->getSetting().get(PlannerBoolParam_OptimizeInitialPosture))
//      this->optimizePosture(ini_state_, ini_state, false, is_not_first_kindyn_iteration);
//
//	current_state_ = ini_state_;
//	for (int time_id=0; time_id<this->getSetting().get(PlannerIntParam_NumTimesteps); time_id++) {
//	  this->optimizePosture(current_state_, ref_sequence.dynamicsState(time_id), true, is_not_first_kindyn_iteration);
//	  kinematicsSequence().dynamicsState(time_id) = current_state_;
//	  kinematicsSequence().dynamicsState(time_id).time() = ref_sequence.dynamicsState(time_id).time();
//	}
//
//    if (this->getSetting().get(PlannerBoolParam_DisplayMotion)) { this->displayMotion(); }
//    if (this->getSetting().get(PlannerBoolParam_StoreData)) { this->storeSolution(); }
  }

  void KinematicsOptimizer::addVariableToModel(const OptimizationVariable& opt_var, Model& model, std::vector<Var>& vars)
  {
    opt_var.getValues(mat_lb_, mat_ub_, mat_guess_, size_, variable_type_);
    for (int col_id=0; col_id<opt_var.getNumCols(); col_id++)
      for (int row_id=0; row_id<opt_var.getNumRows(); row_id++)
        switch (variable_type_) {
          case 'C': { vars[opt_var.id(row_id,col_id)] = model.addVar(VarType::Continuous, double(mat_lb_(row_id,col_id)), double(mat_ub_(row_id,col_id)), double(mat_guess_(row_id,col_id))); break; }
          case 'B': { vars[opt_var.id(row_id,col_id)] = model.addVar(VarType::Binary, int(mat_lb_(row_id,col_id)), int(mat_ub_(row_id,col_id)), double(mat_guess_(row_id,col_id))); break; }
          default: { throw std::runtime_error("At addVariableToModel, variable type not handled"); }
        }

    for (int col_id=0; col_id<opt_var.getNumCols(); col_id++)
      for (int row_id=0; row_id<opt_var.getNumRows(); row_id++)
        vars[opt_var.id(row_id,col_id)].set(SolverDoubleParam_X, mat_guess_(row_id,col_id));
  }

  void KinematicsOptimizer::saveSolution(OptimizationVariable& opt_var)
  {
	mat_guess_.resize(opt_var.getNumRows(), opt_var.getNumCols());
	mat_guess_.setZero();
    for (int col_id=0; col_id<opt_var.getNumCols(); col_id++) {
      for (int row_id=0; row_id<opt_var.getNumRows(); row_id++)
    	    mat_guess_(row_id,col_id) = solution_(opt_var.id(row_id,col_id));
    }
    opt_var.setGuessValue(mat_guess_);
  }

//  void KinematicsOptimizer::displayMotion()
//  {
//	DynamicsState state0, state1, state2 = ini_state_;
//	for (int time_id=0; time_id<this->getSetting().get(PlannerIntParam_NumTimesteps); time_id++) {
//	  if (time_id==0) {
//		state0 = ini_state_;
//		state1 = kinematicsSequence().dynamicsState(time_id);
//	  } else {
//		state0 = kinematicsSequence().dynamicsState(time_id-1);
//		state1 = kinematicsSequence().dynamicsState(time_id);
//	  }
//	  for (int sample_id=0; sample_id<this->getSetting().get(PlannerIntParam_NumSubsamples); sample_id++) {
//		double factor = double(sample_id) / double(this->getSetting().get(PlannerIntParam_NumSubsamples));
//		int size = state2.jointPositions().size()-3;
//		state2.jointPositions().head(size) = state0.jointPositions().head(size) + factor*(state1.jointPositions().head(size) - state0.jointPositions().head(size));
//		state2.baseOrientation() = state0.baseOrientation().slerp(factor, state1.baseOrientation());
//		kin_interface_->displayPosture(state2, kinematicsSequence().dynamicsState(time_id).time()/this->getSetting().get(PlannerIntParam_NumSubsamples));
//	  }
//	}
//  }
//
//  void KinematicsOptimizer::storeSolution()
//  {
//    try
//    {
//    	  YAML::Node qcqp_cfg;
//
//    	  // config parameters
//    	  qcqp_cfg["time_step"] = this->getSetting().get(PlannerDoubleParam_TimeStep);
//    	  qcqp_cfg["time_horizon"] = this->getSetting().get(PlannerDoubleParam_TimeHorizon);
//    	  qcqp_cfg["num_subsamples"] = this->getSetting().get(PlannerIntParam_NumSubsamples);
//
//    	  // saving initial state
//    	  qcqp_cfg["initial_robot_configuration"]["center_of_mass"] = ini_state_.centerOfMass();
//    	  qcqp_cfg["initial_robot_configuration"]["joint_positions"] = ini_state_.jointPositions();
//    	  Eigen::Vector4d base_ori; base_ori << ini_state_.baseOrientation().w(), ini_state_.baseOrientation().vec();
//    	  qcqp_cfg["initial_robot_configuration"]["base_orientation"] = base_ori;
//
//    	  // saving dynamic sequence
//    	  for (int time_id=0; time_id<this->getSetting().get(PlannerIntParam_NumTimesteps); time_id++) {
//    		qcqp_cfg["dyn_state_"+std::to_string(time_id)]["time"] = kinematicsSequence().dynamicsState(time_id).time();
//        qcqp_cfg["dyn_state_"+std::to_string(time_id)]["center_of_mass"] = kinematicsSequence().dynamicsState(time_id).centerOfMass();
//        qcqp_cfg["dyn_state_"+std::to_string(time_id)]["joint_positions"] = kinematicsSequence().dynamicsState(time_id).jointPositions();
//        qcqp_cfg["dyn_state_"+std::to_string(time_id)]["joint_velocities"] = kinematicsSequence().dynamicsState(time_id).jointVelocities();
//        qcqp_cfg["dyn_state_"+std::to_string(time_id)]["joint_accelerations"] = kinematicsSequence().dynamicsState(time_id).jointAccelerations();
//        Eigen::Vector4d base_ori; base_ori << kinematicsSequence().dynamicsState(time_id).baseOrientation().w(), kinematicsSequence().dynamicsState(time_id).baseOrientation().vec();
//        qcqp_cfg["dyn_state_"+std::to_string(time_id)]["base_orientation"] = base_ori;
//    	  }
//      std::ofstream file_out(this->getSetting().get(PlannerStringParam_SaveKinematicsFile));
//      file_out << qcqp_cfg;
//    }
//    catch (YAML::ParserException &e) { std::cout << e.what() << "\n"; }
//  }
//
//  void KinematicsOptimizer::loadSolution(const std::string& load_file)
//  {
//    try
//	{
//    	  YAML::Node qcqp_cfg = YAML::LoadFile(load_file.c_str());
//
//    	  // config parameters
//    	  readParameter(qcqp_cfg, "time_step", this->getSetting().get(PlannerDoubleParam_TimeStep));
//    	  readParameter(qcqp_cfg, "time_horizon", this->getSetting().get(PlannerDoubleParam_TimeHorizon));
//    	  readParameter(qcqp_cfg, "num_subsamples", this->getSetting().get(PlannerIntParam_NumSubsamples));
//    	  this->getSetting().get(PlannerIntParam_NumTimesteps) = std::floor(this->getSetting().get(PlannerDoubleParam_TimeHorizon)/this->getSetting().get(PlannerDoubleParam_TimeStep));
//    	  kinematicsSequence().resize(this->getSetting().get(PlannerIntParam_NumTimesteps));
//
//	  // loading initial state
//    	  YAML::Node ini_params = qcqp_cfg["initial_robot_configuration"];
//    	  ini_state_.time() = this->getSetting().get(PlannerDoubleParam_TimeStep);
//	  readParameter(ini_params, "center_of_mass", ini_state_.centerOfMass());
//	  readParameter(ini_params, "joint_positions", ini_state_.jointPositions());
//	  Eigen::Vector4d v = readParameter<Eigen::Vector4d>(ini_params, "base_orientation");
//	  ini_state_.baseOrientation() = Eigen::Quaternion<double>(v[0], v[1], v[2], v[3]);
//
//	  // loading dynamic sequence
//	  for (int time_id=0; time_id<this->getSetting().get(PlannerIntParam_NumTimesteps); time_id++) {
//		YAML::Node dyn_params = qcqp_cfg["dyn_state_"+std::to_string(time_id)];
//		readParameter(dyn_params, "time", kinematicsSequence().dynamicsState(time_id).time());
//		readParameter(dyn_params, "center_of_mass", kinematicsSequence().dynamicsState(time_id).centerOfMass());
//		readParameter(dyn_params, "joint_positions", kinematicsSequence().dynamicsState(time_id).jointPositions());
//		readParameter(dyn_params, "joint_velocities", kinematicsSequence().dynamicsState(time_id).jointVelocities());
//        readParameter(dyn_params, "joint_accelerations", kinematicsSequence().dynamicsState(time_id).jointAccelerations());
//		Eigen::Vector4d v = readParameter<Eigen::Vector4d>(ini_params, "base_orientation");
//		kinematicsSequence().dynamicsState(time_id).baseOrientation() = Eigen::Quaternion<double>(v[0], v[1], v[2], v[3]);
//	  }
//	}
//    catch (std::runtime_error& e)
//    {
//      std::cout << "Error reading parameter ["<< e.what() << "] at file: [" << __FILE__ << "]" << std::endl << std::endl;
//    }
//  }
//
//  void KinematicsOptimizer::displayMotion(PlannerSetting& planner_setting, KinematicsInterface* kin_interface, const std::string& load_file)
//  {
//	kin_interface_ = kin_interface;
//	planner_setting_ = &planner_setting;
//	this->loadSolution(load_file);
//	this->displayMotion();
//  }

}