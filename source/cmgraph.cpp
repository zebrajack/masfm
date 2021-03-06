#include "cmgraph.hpp"

#pragma warning( push )
# pragma warning (disable:4819)
# pragma warning (disable:4251)
# pragma warning (disable:4355)
#ifdef LIBGLOG_IS_STATIC //see https://github.com/google/glog/issues/103
#define GOOGLE_GLOG_DLL_DECL
#endif
#include "ceres/ceres.h"
#include "ceres/rotation.h"
#pragma warning( pop )

#include "lch.hpp"

namespace cmg {

	template<typename T>
	static inline void transposeInPlace3x3(T R[9])
	{
		std::swap(R[1], R[3]);
		std::swap(R[2], R[6]);
		std::swap(R[5], R[7]);
	}

	template<typename T>
	static inline void multAxAddb(const T A[9], const T x[3], const T b[3], T out[3]) //A is col-major
	{
		out[0] = A[0]*x[0] + A[3]*x[1] + A[6]*x[2] + b[0];
		out[1] = A[1]*x[0] + A[4]*x[1] + A[7]*x[2] + b[1];
		out[2] = A[2]*x[0] + A[5]*x[1] + A[8]*x[2] + b[2];
	}

	//return out=-R*t
	template<typename T>
	static inline void negRt(const T R[9], const T t[3], T out[3]) //R is col-major
	{
		out[0] = -( R[0]*t[0] + R[3]*t[1] + R[6]*t[2] );
		out[1] = -( R[1]*t[0] + R[4]*t[1] + R[7]*t[2] );
		out[2] = -( R[2]*t[0] + R[5]*t[1] + R[8]*t[2] );
	}

	struct InversePoseFunctor {
		//q = f(p), f(.) inverse pose p to pose q
		template <typename T>
		bool operator()(
			const T* const pose,
			T* residuals) const
		{
			T R[9];
			const T * const t = pose+3;
			ceres::AngleAxisToRotationMatrix(pose, R); //column-major
			transposeInPlace3x3(R);
			
			T *Ri = &R[0];
			T *ti = residuals+3;
			negRt(Ri, t, ti);

			ceres::RotationMatrixToAngleAxis(Ri, residuals);

			return true;
		}
	};

#if 0
	struct MarkerReprojectionError
	{
		const CMGraph& G;
		const EID eid;

		MarkerReprojectionError(const CMGraph& G_, const EID& eid_) : G(G_), eid(eid_) {}

		template <typename T>
		bool operator()(
			const T* const view_pose,
			const T* const marker_pose,
			T* residuals) const
		{
			const Mat3x4T mX = CMGraph::marker_X(G.marker_half_size);
			const Mat2x4T& obs = G.edges[eid].u;

			T R_view_in_world[9];
			T *t_view_in_world = view_pose+3;
			ceres::AngleAxisToRotationMatrix(view_pose, R_view_in_world); //column-major
			transposeInPlace3x3(R_view_in_world);
			
			T *R_world_in_view = &R_view_in_world[0];
			T t_world_in_view[3];
			negRt(R_world_in_view, t_view_in_world, t_world_in_view);

			for(int i=0; i<4; ++i) {
				const T pm[3] = { T(mX(0, i)), T(mX(1, i)), T(mX(2, i)) };

				T pw[3];
				ceres::AngleAxisRotatePoint(marker_pose, pm, pw);
				pw[0] += marker_pose[3];
				pw[1] += marker_pose[4];
				pw[2] += marker_pose[5];

				T pc[3];
				multAxAddb(R_world_in_view, pw, t_world_in_view, pc);

				T U[2];
				G.calib.project(pc, U);

				residuals[i*2+0] = U[0] - T(obs(0, i));
				residuals[i*2+1] = U[1] - T(obs(1, i));
			}
			
			return true;
		}
	};
#endif

	struct MarkerReprojectionError2 {
		const CMGraph& G;
		const EID eid;

		MarkerReprojectionError2(const CMGraph& G_, const EID& eid_) : G(G_), eid(eid_) {}

		template <typename T>
		bool operator()(
			const T* const view_pose_inverse,
			const T* const marker_pose,
			T* residuals) const
		{
			const Mat3x4T mX = CMGraph::marker_X(G.marker_half_size);
			const Mat2x4T& obs = G.edges[eid].u;

			for(int i=0; i<4; ++i) {
				const T pm[3] = { T(mX(0, i)), T(mX(1, i)), T(mX(2, i)) };

				T pw[3];
				ceres::AngleAxisRotatePoint(marker_pose, pm, pw);
				pw[0] += marker_pose[3];
				pw[1] += marker_pose[4];
				pw[2] += marker_pose[5];

				T pc[3];
				ceres::AngleAxisRotatePoint(view_pose_inverse, pw, pc);
				pc[0] += view_pose_inverse[3];
				pc[1] += view_pose_inverse[4];
				pc[2] += view_pose_inverse[5];

				T U[2];
				G.calib.project(pc, U);

				residuals[i*2+0] = U[0] - T(obs(0, i));
				residuals[i*2+1] = U[1] - T(obs(1, i));
			}
			
			return true;
		}

		//only optimize pose of a view
		template <typename T>
		inline bool operator()(
			const T* const view_pose_inverse,
			T* residuals) const
		{
			const Vec6T& mp = G.markers[G.edges[eid].mid].p;
			T marker_pose[6] = { T(mp(0)), T(mp(1)), T(mp(2)), T(mp(3)), T(mp(4)), T(mp(5)) };

			return (*this)(view_pose_inverse, marker_pose, residuals);
		}
	};

	struct FixedMarkerError {
		const Pose& fixed_pose;

		FixedMarkerError(const Pose& p) : fixed_pose(p) {}

		template<typename T>
		bool operator()(
			const T* const pose,
			T* residuals) const
		{
			//TODO: use fixed_pose.Cp.inverse() * (current_pose.p - fixed_pose.p);
			T w_ang(1./fixed_pose.Cp(0,0));
			residuals[0] = w_ang*(pose[0] - T(fixed_pose.p(0)));
			residuals[1] = w_ang*(pose[1] - T(fixed_pose.p(1)));
			residuals[2] = w_ang*(pose[2] - T(fixed_pose.p(2)));
			T w_pos(1./fixed_pose.Cp(3,3));
			residuals[3] = w_pos*(pose[3] - T(fixed_pose.p(3)));
			residuals[4] = w_pos*(pose[4] - T(fixed_pose.p(4)));
			residuals[5] = w_pos*(pose[5] - T(fixed_pose.p(5)));
			return true;
		}
	};

	void CMGraph::report(std::ofstream& out) const
	{
		Eigen::IOFormat fmt(Eigen::FullPrecision, Eigen::DontAlignCols);
		//1. markers
		out << markers.size() << std::endl;
		for(size_t mid=0; mid<markers.size(); ++mid) {
			const Node& m = markers[mid];
			out << m.name << " " << m.p.transpose().format(fmt) << std::endl;
			out << m.Cp.format(fmt) << std::endl;

			out << "\t" << m.vmeids.size();
			for(size_t k=0; k<m.vmeids.size(); ++k) {
				out << " " << m.vmeids[k];
			}
			out << std::endl;
		}
		//2. views
		out << views.size() << std::endl;
		for(size_t vid=0; vid<views.size(); ++vid) {
			const Node& v = views[vid];
			out << v.name << " " << v.p.transpose().format(fmt) << std::endl;
			out << v.Cp.format(fmt) << std::endl;

			out << "\t" << v.vmeids.size();
			for(size_t k=0; k<v.vmeids.size(); ++k) {
				out << " " << v.vmeids[k];
			}
			out << std::endl;
		}
		//3. edges
		out << edges.size() << std::endl;
		for(size_t eid=0; eid<edges.size(); ++eid) {
			const Edge& e = edges[eid];
			out << e.vid << " " << e.mid << std::endl;
		}
	}

	NID CMGraph::newMarker(const Vec6T& p, const Mat6T& Cp, const std::string& name)
	{
		Node node;
		node.p = p;
		node.Cp = Cp;
		node.name = name;
		markers.push_back(node);

		NID mid = static_cast<int>(markers.size())-1;
		name2mid[name]=mid;

		if (verbose) {
			clogi("newMarker m%d: %s\n", mid, name.c_str());
		}
		return mid;
	}

	NID CMGraph::newView(const Vec6T& p, const Mat6T& Cp, const std::string& name)
	{
		Node node;
		node.p = p;
		node.Cp = Cp;
		node.name = name;
		views.push_back(node);

		NID vid = static_cast<int>(views.size())-1;

		if (verbose) {
			clogi("newView v%d: %s\n", vid, name.c_str());
		}
		return vid;
	}

	EID CMGraph::newEdge(const NID vid, const NID mid, const MatT& u)
	{
		Edge edge;
		edge.vid=vid;
		edge.mid=mid;
		edge.u = u;

		EID eid = static_cast<int>(edges.size());
		edges.push_back(edge);

		views[vid].vmeids.push_back(eid);
		markers[mid].vmeids.push_back(eid);

		if (verbose) {
			clogi("newEdge v%d->m%d\n", vid, mid);
		}
		return eid;
	}

	NID CMGraph::addObsFromNewView(
		ObsArray& oa,
		const std::string &view_name,
		const bool addNewMarker/*=true*/)
	{
		//sort by perimeter descending order
		std::sort(oa.begin(), oa.end());

		//find first marker that has been added to the graph before
		NID vid = INVALID_NID;
		Veci mids(oa.size(), INVALID_NID); //mids[i] stores the i-th observation's marker ID
		NID first_mid = INVALID_NID;
		int first_mid_pos = -1;
		for(size_t i=0; i<oa.size(); ++i) {
			const Observation& obs = oa[i];
			Str2Int::const_iterator itr = name2mid.find(obs.name);
			if(itr!=name2mid.end()) {
				mids[i] = itr->second;
				if(first_mid==INVALID_NID) {
					first_mid = itr->second;
					first_mid_pos = static_cast<int>(i);
				}
			}
		}

		if(first_mid==INVALID_NID) return vid;
		Mat4T Tmw = markers[first_mid].toT();
		Mat4T Tcm = oa[first_mid_pos].init_marker_pose.invT();
		Mat4T Tcw = Tmw * Tcm;
		//TODO: use all existing marker observation to init the new view pose
		vid = newView(Pose::T2p(Tcw), Mat6T::Identity(), view_name);

		for(size_t i=0; i<oa.size(); ++i) {
			const Observation& obs = oa[i];
			int midi = mids[i];
			if(midi==INVALID_NID && addNewMarker) { //new markers
				Mat4T Tmc = obs.init_marker_pose.toT();
				Tmw = Tcw * Tmc;
				midi = newMarker(Pose::T2p(Tmw), Mat6T::Identity(), obs.name);
			}
			if(midi!=INVALID_NID)
				newEdge(vid, midi, obs.u);
		}
		return vid;
	}

	bool CMGraph::optimizeNewViewPose(
		ObsArray& oa,
		Node& newView,
		const Precision sigma_u,
		const int max_iter,
		const Precision huber_loss_bandwidth/*=10*/, // +/- 10 pixels
		const Precision error_rel_tol/*=1e-2*/,
		const bool computeCovariance/*=false*/)
	{
		const int nedges_old = edges.size();
		NID vid = addObsFromNewView(oa, "vNow", false);
		const int nedges_new = edges.size();

		if(vid==INVALID_NID) return false;
		bool ret = optimizeOneViewPose(vid,
			sigma_u, max_iter,
			huber_loss_bandwidth, error_rel_tol,
			computeCovariance);
		newView = views[vid];

		for(size_t scan=0; scan<newView.vmeids.size(); ++scan) {
			const EID eid = newView.vmeids[scan];
			const Edge& e = edges[eid];
			Node& nd = markers[e.mid];
			if(eid==nd.vmeids[nd.vmeids.size()-1])
				nd.vmeids.resize(nd.vmeids.size()-1);
			else //this should not happen
				nd.vmeids.erase(
					std::remove(nd.vmeids.begin(), nd.vmeids.end(), eid),
					nd.vmeids.end()
				);
		}
		edges.resize(nedges_old);
		views.resize(views.size()-1);
		return ret;
	}

	bool CMGraph::optimizeOneViewPose(
		const NID vid,
		const Precision sigma_u,
		const int max_iter,
		const Precision huber_loss_bandwidth/*=10*/, // +/- 10 pixels
		const Precision error_rel_tol/*=1e-2*/,
		const bool computeCovariance/*=false*/)
	{
		if(vid==INVALID_NID) return false;
		Node& theView = views[vid];
		Vec6T inv_view_pose = theView.invp();

		//1. push residuals into ceres
		ceres::Problem problem; //TODO: make problem a member variable so this process is faster
		ceres::LossFunction* lossFunc = new ceres::HuberLoss(huber_loss_bandwidth);

		//1.1 marker observation residuals
		for(size_t scan=0; scan<theView.vmeids.size(); ++scan) {
			const EID eid = theView.vmeids[scan];
			const Edge& edge = edges[eid];

			if(vid!=edge.vid) {
				tagle("what?!");
				continue;
			}
			Precision *view_pose_inverse = inv_view_pose.data();

			ceres::CostFunction* costFunc =
				new ceres::AutoDiffCostFunction<MarkerReprojectionError2, 8, 6>
				(new MarkerReprojectionError2(*this, static_cast<int>(eid)));

			problem.AddResidualBlock(costFunc, lossFunc, view_pose_inverse);
		}

		//2. solve
		ceres::Solver::Options options;
		options.linear_solver_type = ceres::DENSE_NORMAL_CHOLESKY; //TODO: SPARSE_SCHUR?
		options.minimizer_progress_to_stdout = (helper::GetOrSetLogLevel()>=helper::LOG_DEBUG);
		options.max_num_iterations = max_iter;
		options.function_tolerance = error_rel_tol;

		ceres::Solver::Summary summary;
		ceres::Solve(options, &problem, &summary);
		logld(summary.BriefReport());

		if(summary.termination_type == ceres::FAILURE)
			return false;

		//3. update
		theView.p = Pose(inv_view_pose).invp();

		if(computeCovariance) {
			ceres::Covariance::Options cov_options;
			cov_options.algorithm_type = ceres::DENSE_SVD; //TODO: switch depends on #parameters

			ceres::Covariance covariance(cov_options);
			std::vector< std::pair<const double*, const double*> > cov_blocks;
			cov_blocks.push_back(std::make_pair(
					inv_view_pose.data(), inv_view_pose.data()));

			if(!covariance.Compute(cov_blocks, &problem)) {
				cloge("BA: covariance.Compute failed!");
			} else {
				const Precision sigma_u2 = sigma_u*sigma_u;

				ceres::AutoDiffCostFunction<InversePoseFunctor, 6, 6> adipf(new InversePoseFunctor());
				covariance.GetCovarianceBlock(inv_view_pose.data(), inv_view_pose.data(), theView.Cp.data());
				Vec6T p;
				Mat6T Jt;
				double const * ip = inv_view_pose.data();
				double *pJt = Jt.data();
				//http://ceres-solver.org/modeling.html#CostFunction::Evaluate__doubleCPCP.doubleP.doublePP
				adipf.Evaluate(&ip, p.data(), &pJt);
				theView.Cp = Jt.transpose() * theView.Cp * Jt * sigma_u2;
			}//covariance.Compute
		}

		if(verbose) {
			clogi("BA: exitflag=%d, #iter=%d, time=%f ms\n",
				summary.termination_type,
				summary.iterations.size(),
				summary.total_time_in_seconds*1000
			);
			clogi("    norm(err)/size(err) = %f -> %f\n",
				summary.initial_cost/summary.num_residuals,
				summary.final_cost/summary.num_residuals
			);
		}

		return true;
	}

	bool CMGraph::optimizePose(
		const Precision sigma_u,
		const int max_iter,
		const Precision huber_loss_bandwidth/*=10*/,
		const Precision error_rel_tol/*=1e-2*/,
		const bool computeCovariance/*=false*/)
	{
		//0. collect all view's poses and invert them
		Vec6TArray inv_view_poses(views.size());
		for(size_t vid=0; vid<views.size(); ++vid) {
			inv_view_poses[vid] = views[vid].invp();
		}
		
		//1. push residuals into ceres
		ceres::Problem problem; //TODO: make problem a member variable so this process is faster
		ceres::LossFunction* lossFunc = new ceres::HuberLoss(huber_loss_bandwidth);

		//1.1 marker observation residuals
		for(size_t eid=0; eid<edges.size(); ++eid) {
			const Edge& edge = edges[eid];

			Precision *view_pose_inverse = inv_view_poses[edge.vid].data();
			Precision *marker_pose = markers[edge.mid].p.data();

			ceres::CostFunction* costFunc =
				new ceres::AutoDiffCostFunction<MarkerReprojectionError2, 8, 6, 6>
				(new MarkerReprojectionError2(*this, static_cast<int>(eid)));

			problem.AddResidualBlock(costFunc, lossFunc, view_pose_inverse, marker_pose);
		}

		//1.2 constraint residuals
		Precision *marker_pose = markers[fixed_marker_id].p.data();
		ceres::CostFunction* costFunc =
			new ceres::AutoDiffCostFunction<FixedMarkerError, 6, 6>
			(new FixedMarkerError(fixed_marker_pose));
		problem.AddResidualBlock(costFunc, 0, marker_pose);

		//2. solve
		ceres::Solver::Options options;
		options.linear_solver_type = ceres::DENSE_NORMAL_CHOLESKY; //TODO: SPARSE_SCHUR?
		options.minimizer_progress_to_stdout = (helper::GetOrSetLogLevel()>=helper::LOG_DEBUG);
		options.max_num_iterations = max_iter;
		options.function_tolerance = error_rel_tol;

		ceres::Solver::Summary summary;
		ceres::Solve(options, &problem, &summary);
		logld(summary.BriefReport());

		if(summary.termination_type == ceres::FAILURE)
			return false;

		//3. update
		for(size_t eid=0; eid<edges.size(); ++eid) {
			const Edge& edge = edges[eid];
			Node &view = views[edge.vid];
			view.p = Pose(inv_view_poses[edge.vid]).invp(); //update the view pose
		}

		if(computeCovariance) {
			ceres::Covariance::Options cov_options;
			cov_options.algorithm_type = ceres::DENSE_SVD; //TODO: switch depends on #parameters

			ceres::Covariance covariance(cov_options);
			std::vector< std::pair<const double*, const double*> > cov_blocks;
			for(size_t mid=0; mid<markers.size(); ++mid) {
				Node& marker = markers[mid];
				cov_blocks.push_back(std::make_pair(
					marker.p.data(), marker.p.data()));
			}
			for(size_t vid=0; vid<views.size(); ++vid) {
				cov_blocks.push_back(std::make_pair(
					inv_view_poses[vid].data(), inv_view_poses[vid].data()));
			}

			if(!covariance.Compute(cov_blocks, &problem)) {
				cloge("BA: covariance.Compute failed!");
			} else {
				const Precision sigma_u2 = sigma_u*sigma_u;

				for(size_t mid=0; mid<markers.size(); ++mid) {
					Node& marker = markers[mid];
					//http://ceres-solver.org/solving.html#GetCovarianceBlock__doubleCP.doubleCP.doublePC
					covariance.GetCovarianceBlock(marker.p.data(), marker.p.data(), marker.Cp.data());
					marker.Cp *= sigma_u2;
				}

				ceres::AutoDiffCostFunction<InversePoseFunctor, 6, 6> adipf(new InversePoseFunctor());
				for(size_t vid=0; vid<views.size(); ++vid) {
					Node& view = views[vid];
					covariance.GetCovarianceBlock(inv_view_poses[vid].data(), inv_view_poses[vid].data(), view.Cp.data());
					Vec6T p;
					Mat6T Jt;
					double const * ip = inv_view_poses[vid].data();
					double *pJt = Jt.data();
					//http://ceres-solver.org/modeling.html#CostFunction::Evaluate__doubleCPCP.doubleP.doublePP
					adipf.Evaluate(&ip, p.data(), &pJt);
					view.Cp = Jt.transpose() * view.Cp * Jt * sigma_u2;
				}
			}//covariance.Compute
		}

		if(verbose) {
			clogi("BA: exitflag=%d, #iter=%d, time=%f ms\n",
				summary.termination_type,
				summary.iterations.size(),
				summary.total_time_in_seconds*1000
			);
			clogi("    norm(err)/size(err) = %f -> %f\n",
				summary.initial_cost/summary.num_residuals,
				summary.final_cost/summary.num_residuals
			);
		}

		return true;
	}

	NID CMGraph::setFixedMarker(const std::string &fixed_marker_name,
			const Precision p_ang/*=1e-4*/,
			const Precision p_pos/*=1e-2*/)
	{
		Vec6T fixed_cov;
		fixed_cov << p_ang, p_ang, p_ang, p_pos, p_pos, p_pos;
		fixed_marker_id = newMarker(
			Vec6T::Zero(),
			fixed_cov.asDiagonal(), fixed_marker_name);
		fixed_marker_pose = markers[fixed_marker_id];
		return fixed_marker_id;
	}

	//fix input observations and camera intrinsic parameters,
	//optimize and output marker and view poses
	void CMGraph::BatchProcess(
		const VecObsArray &frames, /*each frame's Observations' order would be sorted */
		const Calibration &calib,
		CMGraph& G,
		const std::string &fixed_marker_name/*=""*/,
		const Precision p_ang/*=1e-4*/,
		const Precision p_pos/*=1e-2*/,
		const Precision sigma_u/*=0.2*/,
		const int max_iter_per_opt/*=20*/,
		const bool do_covariance_estimation/*=true*/)
	{
		G.calib = calib;

		//1. setup fixed marker
		G.setFixedMarker(fixed_marker_name, p_ang, p_pos);

		//2. process each frame
		Veci frame_order;
		Veci frame_todo;
		range(0, static_cast<int>(frames.size()), 1, frame_order);
		for(size_t i=0; i<frames.size(); ++i) {
			//helper::ScopedTimer timer(helper::Timer::UNIT_MS, "[BatchProcess.addObsFromNewView+optimizePose]");
			const int ith = frame_order[i];
			ObsArray& oa = const_cast<ObsArray&>(frames[ith]);
			std::stringstream ss;
			ss << "v" << ith;
			NID vid = G.addObsFromNewView(oa, ss.str());
			if(vid==INVALID_NID) {
				frame_todo.push_back(ith);
				if(G.verbose) {
					clogi("--------------------delayed view %d\n",ith);
				}
				continue;
			}
			G.optimizePose(sigma_u, max_iter_per_opt);

			if(G.verbose) {
				clogi("#markers=%3d, #views=%3d, #edges=%4d | #parameters=%5d, #residuals=%6d"
					" --------------------added view %d\n",
					G.markers.size(), G.views.size(), G.edges.size(),
					G.nParams(), G.nResiduals(),
					ith
				);
			}
		}

		//3. handle delayed views
		for(size_t i=0; i<frame_todo.size(); ++i) {
			//helper::ScopedTimer timer(helper::Timer::UNIT_MS, "[BatchProcess.addObsFromNewView+optimizePose]");
			const int ith = frame_todo[i];
			ObsArray& oa = const_cast<ObsArray&>(frames[ith]);
			std::stringstream ss;
			ss << "v" << ith;
			NID vid = G.addObsFromNewView(oa, ss.str());
			if(vid==INVALID_NID) {
				clogi("view %d is not connected to the current graph, ignored!\n", ith);
				continue; //TODO: better way to initialize the cmgraph, maybe using spanning tree
			}
			G.optimizePose(sigma_u, max_iter_per_opt);

			if(G.verbose) {
				clogi("#markers=%3d, #views=%3d, #edges=%4d | #parameters=%5d, #residuals=%6d"
					" --------------------added delayed view %d\n",
					G.markers.size(), G.views.size(), G.edges.size(),
					G.nParams(), G.nResiduals(),
					ith
				);
			}
		}

		//4. get covariance
		if(do_covariance_estimation)
		{
			helper::ScopedTimer timer(helper::Timer::UNIT_MS, "[BatchProcess.optimizePose covariance]");
			G.optimizePose(sigma_u, 1, 10, 0.01, true);
			if(G.verbose) {
				clogi("========================compute all covariance\n");
			}
		}
	}

}