//AlignPoints.cc - A part of DICOMautomaton 2019. Written by hal clark.

#include <asio.hpp>
#include <algorithm>
#include <optional>
#include <fstream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set> 
#include <stdexcept>
#include <string>    
#include <utility>            //Needed for std::pair.
#include <vector>

#ifdef DCMA_USE_EIGEN    
    #include <eigen3/Eigen/Dense>
    #include <eigen3/Eigen/Eigenvalues>
    #include <eigen3/Eigen/SVD>
#endif

#include "../Structs.h"
#include "../Regex_Selectors.h"
#include "../Thread_Pool.h"
#include "AlignPoints.h"
#include "Explicator.h"       //Needed for Explicator class.
#include "YgorImages.h"
#include "YgorMath.h"         //Needed for vec3 class.
#include "YgorMisc.h"         //Needed for FUNCINFO, FUNCWARN, FUNCERR macros.
#include "YgorStats.h"        //Needed for Stats:: namespace.
#include "YgorString.h"       //Needed for GetFirstRegex(...)


struct
AffineTransform {

    private:
        // The top-left 3x3 sub-matrix is a rotation matrix. The top right-most column 3-vector is a translation vector.
        //
        //     (0,0)    (1,0)    (2,0)  |  (3,0)                               |                  
        //     (0,1)    (1,1)    (2,1)  |  (3,1)             linear transform  |  translation     
        //     (0,2)    (1,2)    (2,2)  |  (3,2)     =        (inc. scaling)   |                  
        //     ---------------------------------           ------------------------------------   
        //     (0,3)    (1,3)    (2,3)  |  (3,3)                 (zeros)       |  projection     
        //
        // Note that the bottom row must remain unaltered to be an Affine transform.
        //
        // The relative scale of transformed vectors is controlled by the magnitude of the linear transform column
        // vectors.
        //
        std::array< std::array<double, 4>, 4> t = {{ std::array<double,4>{{ 1.0, 0.0, 0.0, 0.0 }},
                                                     std::array<double,4>{{ 0.0, 1.0, 0.0, 0.0 }},
                                                     std::array<double,4>{{ 0.0, 0.0, 1.0, 0.0 }},
                                                     std::array<double,4>{{ 0.0, 0.0, 0.0, 1.0 }} }};

    public:
        // Accessors.
        double &
        coeff(long int i, long int j){
            if(!isininc(0L,i,3L) || !isininc(0L,j,2L)) throw std::invalid_argument("Tried to access fixed coefficients."
                                                                                   " Refusing to continue.");
            return this->t[i][j];
        }

        // Apply the (full) transformation to a vec3.
        vec3<double> 
        apply_to(const vec3<double> &in) const {
            const auto x = (in.x * this->t[0][0]) + (in.y * this->t[1][0]) + (in.z * this->t[2][0]) + (1.0 * this->t[3][0]);
            const auto y = (in.x * this->t[0][1]) + (in.y * this->t[1][1]) + (in.z * this->t[2][1]) + (1.0 * this->t[3][1]);
            const auto z = (in.x * this->t[0][2]) + (in.y * this->t[1][2]) + (in.z * this->t[2][2]) + (1.0 * this->t[3][2]);
            const auto w = (in.x * this->t[0][3]) + (in.y * this->t[1][3]) + (in.z * this->t[2][3]) + (1.0 * this->t[3][3]);

            if(w != 1.0) throw std::runtime_error("Transformation is not Affine. Refusing to continue.");

            return vec3<double>(x, y, z);
        }

        // Apply the transformation to a point cloud.
        void
        apply_to(point_set<double> &in){
            for(auto &p : in.points){
                p = this->apply_to(p);
            }
            return;
        }

        // Write the transformation to a stream.
        bool
        write_to( std::ostream &os ){
            // Maximize precision prior to emitting the vertices.
            const auto original_precision = os.precision();
            os.precision( std::numeric_limits<double>::digits10 + 1 );
            os << this->t[0][0] << " " << this->t[1][0] << " " << this->t[2][0] << " " << this->t[3][0] << std::endl;
            os << this->t[0][1] << " " << this->t[1][1] << " " << this->t[2][1] << " " << this->t[3][1] << std::endl;
            os << this->t[0][2] << " " << this->t[1][2] << " " << this->t[2][2] << " " << this->t[3][2] << std::endl;
            os << this->t[0][3] << " " << this->t[1][3] << " " << this->t[2][3] << " " << this->t[3][3] << std::endl;

            // Reset the precision on the stream.
            os.precision( original_precision );
            os.flush();
            return(!os.fail());
        }

        // Read the transformation from a stream.
        bool
        read_from( std::istream &is ){
            is >> this->t[0][0] >> this->t[1][0] >> this->t[2][0] >> this->t[3][0];
            is >> this->t[0][1] >> this->t[1][1] >> this->t[2][1] >> this->t[3][1];
            is >> this->t[0][2] >> this->t[1][2] >> this->t[2][2] >> this->t[3][2];
            is >> this->t[0][3] >> this->t[1][3] >> this->t[2][3] >> this->t[3][3];

            const auto machine_eps = std::sqrt( std::numeric_limits<double>::epsilon() );
            if( (std::fabs(this->t[0][3] - 0.0) > machine_eps)
            ||  (std::fabs(this->t[1][3] - 0.0) > machine_eps)
            ||  (std::fabs(this->t[2][3] - 0.0) > machine_eps)
            ||  (std::fabs(this->t[3][3] - 1.0) > machine_eps) ){
                FUNCWARN("Unable to read transformation; not Affine");
                return false;
            }

            return(!is.fail());
        }

};


// This routine performs a simple centroid-based alignment.
//
// The resultant transformation is a rotation-less shift so the point cloud centres-of-mass overlap.
//
// Note that this routine only identifies a transform, it does not implement it by altering the point clouds.
//
static
std::optional<AffineTransform>
AlignViaCentroid(const point_set<double> & moving,
                 const point_set<double> & stationary ){
    AffineTransform t;

    // Compute the centroid for both point clouds.
    const auto centroid_s = stationary.Centroid();
    const auto centroid_m = moving.Centroid();

    const auto dcentroid = (centroid_s - centroid_m);
    t.coeff(3,0) = dcentroid.x;
    t.coeff(3,1) = dcentroid.y;
    t.coeff(3,2) = dcentroid.z;

    return t;
}
    
#ifdef DCMA_USE_EIGEN
// This routine performs a PCA-based alignment.
//
// First, the moving point cloud is translated the moving point cloud so the centre of mass aligns to the reference
// point cloud, performs PCA separately on the reference and moving point clouds, compute distribution moments along
// each axis to determine the direction, and then rotates the moving point cloud so the principle axes coincide.
//
// Note that this routine only identifies a transform, it does not implement it by altering the point clouds.
//
static
std::optional<AffineTransform>
AlignViaPCA(const point_set<double> & moving,
            const point_set<double> & stationary ){
    AffineTransform t;

    // Compute the centroid for both point clouds.
    const auto centroid_s = stationary.Centroid();
    const auto centroid_m = moving.Centroid();
    
    // Compute the PCA for both point clouds.
    struct pcomps {
        vec3<double> pc1;
        vec3<double> pc2;
        vec3<double> pc3;
    };
    const auto est_PCA = [](const point_set<double> &ps) -> pcomps {
        // Determine the three most prominent unit vectors via PCA.
        Eigen::MatrixXd mat;
        const size_t mat_rows = ps.points.size();
        const size_t mat_cols = 3;
        mat.resize(mat_rows, mat_cols);
        {
            size_t i = 0;
            for(const auto &v : ps.points){
                mat(i, 0) = static_cast<double>(v.x);
                mat(i, 1) = static_cast<double>(v.y);
                mat(i, 2) = static_cast<double>(v.z);
                ++i;
            }
        }

        Eigen::MatrixXd centered = mat.rowwise() - mat.colwise().mean();
        Eigen::MatrixXd cov = centered.adjoint() * centered;
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(cov);
        
        Eigen::VectorXd evals = eig.eigenvalues();
        Eigen::MatrixXd evecs = eig.eigenvectors().real();

        pcomps out;
        out.pc1 = vec3<double>( evecs(0,0), evecs(1,0), evecs(2,0) ).unit();
        out.pc2 = vec3<double>( evecs(0,1), evecs(1,1), evecs(2,1) ).unit();
        out.pc3 = vec3<double>( evecs(0,2), evecs(1,2), evecs(2,2) ).unit();
        return out;
    };

    const auto pcomps_stationary = est_PCA(stationary);
    const auto pcomps_moving = est_PCA(moving);

    // Compute centroid-centered third-order moments (i.e., skew) along each component and use them to reorient the principle components.
    // The third order is needed since the first-order (mean) is eliminated via centroid-shifting, and the second order
    // (variance) cannot differentiate positive and negative directions.
    const auto reorient_pcomps = [](const vec3<double> &centroid,
                                    const pcomps &comps,
                                    const point_set<double> &ps) {

        Stats::Running_Sum<double> rs_pc1;
        Stats::Running_Sum<double> rs_pc2;
        Stats::Running_Sum<double> rs_pc3;
        for(const auto &v : ps.points){
            const auto sv = (v - centroid);

            const auto proj_pc1 = sv.Dot(comps.pc1);
            rs_pc1.Digest( std::pow(proj_pc1, 3.0) );
            const auto proj_pc2 = sv.Dot(comps.pc2);
            rs_pc2.Digest( std::pow(proj_pc2, 3.0) );
            const auto proj_pc3 = sv.Dot(comps.pc3);
            rs_pc3.Digest( std::pow(proj_pc3, 3.0) );
        }

        pcomps out;
        out.pc1 = (comps.pc1 * rs_pc1.Current_Sum()).unit(); // Will be either + or - the original pcomps.
        out.pc2 = (comps.pc2 * rs_pc2.Current_Sum()).unit(); // Will be either + or - the original pcomps.
        out.pc3 = (comps.pc3 * rs_pc3.Current_Sum()).unit(); // Will be either + or - the original pcomps.


        // Handle 2D degeneracy.
        //
        // If the space is degenerate with all points being coplanar, then the first (strongest) principle component
        // will be orthogonal to the plane and the corresponding moment will be zero. The other two reoriented
        // components will still be valid, and the underlying principal component is correct; we just don't know the
        // direction because the moment is zero. However, we can determine it in a consistent way by relying on the
        // other two (valid) adjusted components.
        if( !(out.pc1.isfinite())
        &&  out.pc2.isfinite() 
        &&  out.pc3.isfinite() ){
            out.pc1 = out.pc3.Cross( out.pc2 ).unit();
        }

        // Handle 1D degeneracy (somewhat).
        //
        // If the space is degenerate with all points being colinear, then the first two principle components
        // will be randomly oriented orthgonal to the line and the last component will be tangential to the line
        // with a direction derived from the moment. We cannot unambiguously recover the first two components, but we
        // can at least fall back on the original principle components.
        if( !(out.pc1.isfinite()) ) out.pc1 = comps.pc1;
        if( !(out.pc2.isfinite()) ) out.pc2 = comps.pc2;
        //if( !(out.pc3.isfinite()) ) out.pc3 = comps.pc3;

        return out;
    };

    const auto reoriented_pcomps_stationary = reorient_pcomps(centroid_s,
                                                              pcomps_stationary,
                                                              stationary);
    const auto reoriented_pcomps_moving = reorient_pcomps(centroid_m,
                                                          pcomps_moving,
                                                          moving);

    FUNCINFO("Stationary point cloud:");
    FUNCINFO("    centroid             : " << centroid_s);
    FUNCINFO("    pcomp_pc1            : " << pcomps_stationary.pc1);
    FUNCINFO("    pcomp_pc2            : " << pcomps_stationary.pc2);
    FUNCINFO("    pcomp_pc3            : " << pcomps_stationary.pc3);
    FUNCINFO("    reoriented_pcomp_pc1 : " << reoriented_pcomps_stationary.pc1);
    FUNCINFO("    reoriented_pcomp_pc2 : " << reoriented_pcomps_stationary.pc2);
    FUNCINFO("    reoriented_pcomp_pc3 : " << reoriented_pcomps_stationary.pc3);

    FUNCINFO("Moving point cloud:");
    FUNCINFO("    centroid             : " << centroid_m);
    FUNCINFO("    pcomp_pc1            : " << pcomps_moving.pc1);
    FUNCINFO("    pcomp_pc2            : " << pcomps_moving.pc2);
    FUNCINFO("    pcomp_pc3            : " << pcomps_moving.pc3);
    FUNCINFO("    reoriented_pcomp_pc1 : " << reoriented_pcomps_moving.pc1);
    FUNCINFO("    reoriented_pcomp_pc2 : " << reoriented_pcomps_moving.pc2);
    FUNCINFO("    reoriented_pcomp_pc3 : " << reoriented_pcomps_moving.pc3);

    // Determine the linear transformation that will align the reoriented principle components.
    //
    // If we assemble the orthonormal principle component vectors for each cloud into a 3x3 matrix (i.e., three column
    // vectors) we get an orthonormal matrix. The transformation matrix 'A' needed to transform the moving matrix 'M'
    // into the stationary matrix 'S' can be found from $S = AM$. Since M is orthonormal, $M^{-1}$ always exists and
    // also $M^{-1} = M^{T}$. So $A = SM^{T}$.

    {
        Eigen::Matrix3d S;
        S(0,0) = reoriented_pcomps_stationary.pc1.x;
        S(1,0) = reoriented_pcomps_stationary.pc1.y;
        S(2,0) = reoriented_pcomps_stationary.pc1.z;

        S(0,1) = reoriented_pcomps_stationary.pc2.x;
        S(1,1) = reoriented_pcomps_stationary.pc2.y;
        S(2,1) = reoriented_pcomps_stationary.pc2.z;

        S(0,2) = reoriented_pcomps_stationary.pc3.x;
        S(1,2) = reoriented_pcomps_stationary.pc3.y;
        S(2,2) = reoriented_pcomps_stationary.pc3.z;

        Eigen::Matrix3d M;
        M(0,0) = reoriented_pcomps_moving.pc1.x;
        M(1,0) = reoriented_pcomps_moving.pc1.y;
        M(2,0) = reoriented_pcomps_moving.pc1.z;

        M(0,1) = reoriented_pcomps_moving.pc2.x;
        M(1,1) = reoriented_pcomps_moving.pc2.y;
        M(2,1) = reoriented_pcomps_moving.pc2.z;

        M(0,2) = reoriented_pcomps_moving.pc3.x;
        M(1,2) = reoriented_pcomps_moving.pc3.y;
        M(2,2) = reoriented_pcomps_moving.pc3.z;

        auto A = S * M.transpose();
        // Force the transform to be the identity for debugging.
        //A << 1.0, 0.0, 0.0,
        //     0.0, 1.0, 0.0, 
        //     0.0, 0.0, 1.0;

        t.coeff(0,0) = A(0,0);
        t.coeff(0,1) = A(1,0);
        t.coeff(0,2) = A(2,0);

        t.coeff(1,0) = A(0,1);
        t.coeff(1,1) = A(1,1);
        t.coeff(1,2) = A(2,1);

        t.coeff(2,0) = A(0,2);
        t.coeff(2,1) = A(1,2);
        t.coeff(2,2) = A(2,2);

        // Work out the translation vector.
        //
        // Because the centroid is not explicitly subtracted, we have to incorporate the subtraction into the translation term.
        // Ideally we would perform $A * (M - centroid_{M}) + centroid_{S}$ explicitly; to emulate this, we can rearrange to find
        // $A * M + \left( centroid_{S} - A * centroid_{M} \right) \equiv A * M + b$ where $b = centroid_{S} - A * centroid_{M}$ is the
        // necessary translation term.
        {
            Eigen::Vector3d e_centroid_m(centroid_m.x, centroid_m.y, centroid_m.z);
            auto A_e_centroid_m = A * e_centroid_m; 

            t.coeff(3,0) = centroid_s.x - A_e_centroid_m(0);
            t.coeff(3,1) = centroid_s.y - A_e_centroid_m(1);
            t.coeff(3,2) = centroid_s.z - A_e_centroid_m(2);
        }
    }

    //FUNCINFO("Final linear transform:");
    //FUNCINFO("    ( " << t.coeff(0,0) << "  " << t.coeff(1,0) << "  " << t.coeff(2,0) << " )");
    //FUNCINFO("    ( " << t.coeff(0,1) << "  " << t.coeff(1,1) << "  " << t.coeff(2,1) << " )");
    //FUNCINFO("    ( " << t.coeff(0,2) << "  " << t.coeff(1,2) << "  " << t.coeff(2,2) << " )");
    //FUNCINFO("Final translation:");
    //FUNCINFO("    ( " << t.coeff(3,0) << " )");
    //FUNCINFO("    ( " << t.coeff(3,1) << " )");
    //FUNCINFO("    ( " << t.coeff(3,2) << " )");
    //FUNCINFO("Final Affine transformation:");
    //t.write_to(std::cout);

    return t;
}
#endif // DCMA_USE_EIGEN


#ifdef DCMA_USE_EIGEN
// This routine performs an exhaustive iterative closest point (ICP) alignment.
//
// Note that this routine only identifies a transform, it does not implement it by altering the point clouds.
//
static
std::optional<AffineTransform>
AlignViaExhaustiveICP( const point_set<double> & moving,
                       const point_set<double> & stationary,
                       long int max_icp_iters = 100,
                       double f_rel_tol = std::numeric_limits<double>::quiet_NaN() ){

    // The WIP transformation.
    AffineTransform t;

    // The transformation that resulted in the lowest cost estimate so far.
    AffineTransform t_best;
    double f_best = std::numeric_limits<double>::infinity();

    // Compute the centroid for both point clouds.
    const auto centroid_s = stationary.Centroid();
    const auto centroid_m = moving.Centroid();

    point_set<double> working(moving);
    point_set<double> corresp(moving);
    
    // Prime the transformation using a simplistic alignment.
    //
    // Note: The initial transformation will only be used to establish correspondence in the first iteration, so it
    // might be tolerable to be somewhat coarse. Note, however, that a bad initial guess (in the sense that the true
    // optimal alignment is impeded by many local minima) will certainly negatively impact the convergence rate, and may
    // actually make it impossible to find the true alignment using this alignment method. Therefore, the PCA method is
    // used by default. If problems are encountered with the PCA method, resorting to the centroid method may be
    // sufficient.
    //
    // Default:
    t = AlignViaPCA(moving, stationary).value();
    //
    // Fallback:
    //t = AlignViaCentroid(moving, stationary).value();

    double f_prev = std::numeric_limits<double>::quiet_NaN();
    for(long int icp_iter = 0; icp_iter < max_icp_iters; ++icp_iter){
        // Copy the original points.
        working.points = moving.points;

        // Apply the current transformation to the working points.
        t.apply_to(working);
        const auto centroid_w = working.Centroid();

        // Exhaustively determine the correspondence between stationary and working points under the current
        // transformation. Note that multiple working points may correspond to the same stationary point.
        const auto N_working_points = working.points.size();
        if(N_working_points != corresp.points.size()) throw std::logic_error("Encountered inconsistent working buffers. Cannot continue.");
        {
            asio_thread_pool tp;
            for(size_t i = 0; i < N_working_points; ++i){
                tp.submit_task([&,i](void) -> void {
                    const auto w_p = working.points[i];
                    double min_sq_dist = std::numeric_limits<double>::infinity();
                    for(const auto &s_p : stationary.points){
                        const auto sq_dist = w_p.sq_dist(s_p);
                        if(sq_dist < min_sq_dist){
                            min_sq_dist = sq_dist;
                            corresp.points[i] = s_p;
                        }
                    }
                }); // thread pool task closure.
            }
        } // Wait until all threads are done.


        ///////////////////////////////////

        // Using the correspondence, estimate the linear transformation that will maximize alignment between
        // centroid-shifted point clouds.
        //
        // Note: the transformation we seek here ignores translations by explicitly subtracting the centroid from each
        // point cloud. Translations will be added into the full transformation later. 
        const auto N_rows = 3;
        const auto N_cols = N_working_points;
        Eigen::MatrixXd S(N_rows, N_cols);
        Eigen::MatrixXd M(N_rows, N_cols);

        for(size_t i = 0; i < N_working_points; ++i){
            // Note: Find the transform using the original point clouds (with a centroid shift) and the updated
            // correspondence information.

            S(0, i) = corresp.points[i].x - centroid_s.x; // The desired point location.
            S(1, i) = corresp.points[i].y - centroid_s.y;
            S(2, i) = corresp.points[i].z - centroid_s.z;

            M(0, i) = moving.points[i].x - centroid_w.x; // The actual point location.
            M(1, i) = moving.points[i].y - centroid_w.y;
            M(2, i) = moving.points[i].z - centroid_w.z;
        }
        auto ST = S.transpose();
        auto MST = M * ST;

        //Eigen::JacobiSVD<Eigen::MatrixXd> SVD(MST, Eigen::ComputeThinU | Eigen::ComputeThinV);
        Eigen::JacobiSVD<Eigen::MatrixXd> SVD(MST, Eigen::ComputeFullU | Eigen::ComputeFullV );
        auto U = SVD.matrixU();
        auto V = SVD.matrixV();

        // Use the SVD result directly.
        //
        // Note that spatial inversions are permitted this way.
        //auto A = U * V.transpose();

        // Attempt to restrict to rotations only.    NOTE: Does not appear to work?
        //Eigen::Matrix3d PI;
        //PI << 1.0 , 0.0 , 0.0,
        //      0.0 , 1.0 , 0.0,
        //      0.0 , 0.0 , ( U * V.transpose() ).determinant();
        //auto A = U * PI * V.transpose();

        // Restrict the solution to rotations only. (Refer to the 'Kabsch algorithm' for more info.)
        Eigen::Matrix3d PI;
        PI << 1.0 , 0.0 , 0.0
            , 0.0 , 1.0 , 0.0
            , 0.0 , 0.0 , ( V * U.transpose() ).determinant();
        auto A = V * PI * U.transpose();

/*
        // Apply the linear transformation to a point directly.
        auto Apply_Rotation = [&](const vec3<double> &v) -> vec3<double> {
            Eigen::Vector3f e_vec3(v.x, v.y, v.z);
            auto new_v = A * e_vec3;
            return vec3<double>( new_v(0), new_v(1), new_v(2) );
        };
*/

        // Transfer the transformation into a full Affine transformation.
        t = AffineTransform();

        // Rotation and scaling components.
        t.coeff(0,0) = A(0,0);
        t.coeff(0,1) = A(1,0);
        t.coeff(0,2) = A(2,0);

        t.coeff(1,0) = A(0,1);
        t.coeff(1,1) = A(1,1);
        t.coeff(1,2) = A(2,1);

        t.coeff(2,0) = A(0,2);
        t.coeff(2,1) = A(1,2);
        t.coeff(2,2) = A(2,2);

        // The complete transformation we have found for bringing the moving points $P_{M}$ into alignment with the
        // stationary points is:
        //
        //   $centroid_{S} + A * \left( P_{M} - centroid_{M} \right)$.
        //
        // Rearranging, an Affine transformation of the form $A * P_{M} + b$ can be written as:
        //
        //   $A * P_{M} + \left( centroid_{S} - A * centroid_{M} \right)$.
        // 
        // Specifically, the transformed moving point cloud centroid component needs to be pre-subtracted for each
        // vector $P_{M}$ to anticipate not having an explicit centroid subtraction step prior to applying the
        // scale/rotation matrix.
        {
            Eigen::Vector3d e_centroid(centroid_m.x, centroid_m.y, centroid_m.z);
            auto A_e_centroid = A * e_centroid; 

            t.coeff(3,0) = centroid_s.x - A_e_centroid(0);
            t.coeff(3,1) = centroid_s.y - A_e_centroid(1);
            t.coeff(3,2) = centroid_s.z - A_e_centroid(2);
        }

        // Evaluate whether the current transformation is sufficient. If so, terminate the loop.
        working.points = moving.points;
        t.apply_to(working);
        double f_curr = 0.0;
        for(size_t i = 0; i < N_working_points; ++i){
            const auto w_p = working.points[i];
            const auto s_p = stationary.points[i];
            const auto dist = s_p.distance(w_p);
            f_curr += dist;
        }

        FUNCINFO("Global distance using correspondence estimated during iteration " << icp_iter << " is " << f_curr);

        if(f_curr < f_best){
            f_best = f_curr;
            t_best = t;
        }
        if( std::isfinite(f_rel_tol) 
        &&  std::isfinite(f_curr)
        &&  std::isfinite(f_prev) ){
            const auto f_rel = std::fabs( (f_prev - f_curr) / f_prev );
            FUNCINFO("The relative change in global distance compared to the last iteration is " << f_rel);
            if(f_rel < f_rel_tol) break;
        }
        f_prev = f_curr;
    }

    // Select the best transformation observed so far.
    t = t_best;

    // Report the transformation and pass it to the user.
    //FUNCINFO("Final Affine transformation:");
    //t.write_to(std::cout);
    return t;
}
#endif // DCMA_USE_EIGEN

#ifdef DCMA_USE_EIGEN
// This routine finds a non-rigid alignment using the 'robust point matching: thin plate spline' algorithm.
//
// TODO: This algorithm is a WIP!
static
std::optional<AffineTransform>
AlignViaTPSRPM(const point_set<double> & moving,
               const point_set<double> & stationary ){
    AffineTransform t;

    // Compute the centroid for both point clouds.
    const auto centroid_s = stationary.Centroid();
    const auto centroid_m = moving.Centroid();
    
    const double T_step = 0.93; // Should be [0.9:0.99] or so.

    // Find the largest 'square distance' between (all) points and the average separation of nearest-neighbour points
    // (in the moving cloud). This info is needed to tune the annealing energy to ensure (1) deformations can initially
    // 'reach' across the point cloud, and (2) deformations are not considered much below the nearest-neighbour spacing
    // (i.e., overfitting).

    const auto N_moving_points = moving.points.size();
    const auto N_stationary_points = stationary.points.size();
    double mean_nn_sq_dist = std::numeric_limits<double>::quiet_NaN();
    double max_sq_dist = 0.0;
    {
        FUNCINFO("Locating mean nearest-neighbour separation in moving point cloud");
        Stats::Running_Sum<double> rs;
        long int count = 0;
        {
            //asio_thread_pool tp;
            for(size_t i = 0; i < N_moving_points; ++i){
                //tp.submit_task([&,i](void) -> void {
                for(size_t j = 0; j < i; ++j){
                    const auto dist = (moving.points[i]).sq_dist( moving.points[j] );
                    rs.Digest(dist);
                    ++count;
                }
                //}); // thread pool task closure.
            }
        }
        mean_nn_sq_dist = rs.Current_Sum() / static_cast<double>( count );

        FUNCINFO("Locating max square-distance between all points");
        {
            asio_thread_pool tp;
            std::mutex saver_printer;
            for(size_t i = 0; i < (N_moving_points + N_stationary_points); ++i){
                tp.submit_task([&,i](void) -> void {
                    for(size_t j = 0; j < i; ++j){
                        const auto A = (i < N_moving_points) ? moving.points[i] : stationary.points[i - N_moving_points];
                        const auto B = (j < N_moving_points) ? moving.points[j] : stationary.points[j - N_moving_points];
                        const auto sq_dist = A.sq_dist(B);
                        if(max_sq_dist < sq_dist){
                            std::lock_guard<std::mutex> lock(saver_printer);
                            max_sq_dist = sq_dist;
                        }
                    }
                }); // thread pool task closure.
            }
        } // Wait until all threads are done.
    }

    double T_start = max_sq_dist;
    double T_end = mean_nn_sq_dist;
    double L_1_start = 1.0;
    double L_2_start = 0.01 * L_1_start;


    // Prepare the transformation and initial correspondence.
    const auto f_tps = [&](const vec3<double> &v) -> vec3<double> {
        
        // TODO: implement TPS transformation class as a REPLACEMENT for this closure.

        return v;
    };

    // Prime the transformation with a rigid registration.

    // TODO -- is this step necessary?


    // TODO: VERIFY THE DIMENSION ARE NOT BACKWARD!

    const auto M_N_rows = N_moving_points + 1;
    const auto M_N_cols = N_stationary_points + 1;
    Eigen::MatrixXd M(M_N_rows, M_N_cols);

    // Update the correspondence.
    const auto update_correspondence = [&](const double T_now) -> void {
        // Non-outlier coefficients.
        for(size_t i = 0; i < N_moving_points; ++i){ // row
            const auto P_moving = moving.points[i];
            const auto P_moved = f_tps(P_moving); // Transform the point.
            for(size_t j = 0; j < N_stationary_points; ++j){ // column
                const auto P_stationary = stationary.points[j];
                const auto dP = P_stationary - P_moved;
                M(i, j) = std::exp(-dP.Dot(dP) / (2.0 * T_now)) / T_now;
            }
        }

        // Moving outlier coefficients.
        {
            const auto i = N_moving_points; // row
            const auto P_moving = moving.points[i];
            for(size_t j = 0; j < N_stationary_points; ++j){ // column
                const auto P_stationary = stationary.points[j];
                const auto dP = P_stationary - P_moving; // Note: intentionally not transformed.
                M(i, j) = std::exp(-dP.Dot(dP) / (2.0 * T_start)) / T_start; // Note: always use initial start temperature.
            }
        }

        // Stationary outlier coefficients.
        for(size_t i = 0; i < N_moving_points; ++i){ // row
            const auto P_moving = moving.points[i];
            const auto P_moved = f_tps(P_moving); // Transform the point.
            const auto j = N_stationary_points; // column
            const auto P_stationary = stationary.points[j];
            const auto dP = P_stationary - P_moved;
            M(i, j) = std::exp(-dP.Dot(dP) / (2.0 * T_start)) / T_start; // Note: always use initial start temperature.
        }

        // Normalize the rows and columns iteratively.
        {
            std::vector<double> row_sums(N_moving_points+1, 0.0);
            std::vector<double> col_sums(N_stationary_points+1, 0.0);
            for(size_t norm_iter = 0; norm_iter < 10; ++norm_iter){

                // Tally the current column sums and re-scale the corespondence coefficients.
                //for(auto &x : col_sums) x = 0.0;
                for(size_t j = 0; j < (N_stationary_points+1); ++j){ // column
                    col_sums[j] = 0.0;
                    for(size_t i = 0; i < (N_moving_points+1); ++i){ // row
                        col_sums[j] += M(i,j);
                    }
                }
                for(size_t j = 0; j < (N_stationary_points+1); ++j){ // column
                    for(size_t i = 0; i < (N_moving_points+1); ++i){ // row
                        M(i,j) = M(i,j) / col_sums[j];
                    }
                }
                
                // Tally the current row sums and re-scale the corespondence coefficients.
                //for(auto &x : row_sums) x = 0.0;
                for(size_t i = 0; i < (N_moving_points+1); ++i){ // row
                    row_sums[i] = 0.0;
                    for(size_t j = 0; j < (N_stationary_points+1); ++j){ // column
                        row_sums[i] += M(i,j);
                    }
                }
                for(size_t i = 0; i < (N_moving_points+1); ++i){ // row
                    for(size_t j = 0; j < (N_stationary_points+1); ++j){ // column
                        M(i,j) = M(i,j) / row_sums[i];
                    }
                }

                FUNCINFO("On normalization iteration " << norm_iter << " the mean col sum was " << Stats::Mean(col_sums));
                FUNCINFO("On normalization iteration " << norm_iter << " the mean row sum was " << Stats::Mean(row_sums));
            }
        }

        return;
    };

    // Update the transformation.
    const auto update_transformation = [&](const double T_now) -> void {

        // TODO.

        return;
    };

    // Anneal deterministically.
    for(double T_now = T_start; T_now >= T_end; T_now *= T_step){
        const double L_1 = T_now * L_1_start;
        const double L_2 = T_now * L_2_start;

        for(size_t iter_at_fixed_T = 0; iter_at_fixed_T < 5; ++iter_at_fixed_T){
            // Update correspondence matrix.
            update_correspondence(T_now);

            // Update transformation.
            update_transformation(T_now);
        }
    }

    return t;
}
#endif // DCMA_USE_EIGEN


OperationDoc OpArgDocAlignPoints(void){
    OperationDoc out;
    out.name = "AlignPoints";

    out.desc = 
        "This operation aligns (i.e., 'registers') a 'moving' point cloud to a 'stationary' (i.e., 'reference') point cloud.";
        
    out.notes.emplace_back(
        "The 'moving' point cloud is transformed after the final transformation has been estimated."
        " It should be copied if a pre-transformed copy is required."
    );
        
#ifdef DCMA_USE_EIGEN
#else
    out.notes.emplace_back(
        "Functionality provided by Eigen has been disabled. The available transformation methods have been reduced."
    );
#endif

    out.args.emplace_back();
    out.args.back() = PCWhitelistOpArgDoc();
    out.args.back().name = "MovingPointSelection";
    out.args.back().default_val = "last";
    out.args.back().desc = "The point cloud that will be transformed. "_s
                         + out.args.back().desc;


    out.args.emplace_back();
    out.args.back() = PCWhitelistOpArgDoc();
    out.args.back().name = "ReferencePointSelection";
    out.args.back().default_val = "last";
    out.args.back().desc = "The stationary point cloud to use as a reference for the moving point cloud. "_s
                         + out.args.back().desc
                         + " Note that this point cloud is not modified.";


    out.args.emplace_back();
    out.args.back().name = "Method";
    out.args.back().desc = "The alignment algorithm to use."
                           " The following alignment options are available: 'centroid'"
#ifdef DCMA_USE_EIGEN
                           ", 'PCA', and 'exhaustive_icp'"
#endif
                           "."
                           " The 'centroid' option finds a rotationless translation the aligns the centroid"
                           " (i.e., the centre of mass if every point has the same 'mass')"
                           " of the moving point cloud with that of the stationary point cloud."
                           " It is susceptible to noise and outliers, and can only be reliably used when the point"
                           " cloud has complete rotational symmetry (i.e., a sphere). On the other hand, 'centroid'"
                           " alignment should never fail, can handle a large number of points,"
                           " and can be used in cases of 2D and 1D degeneracy."
                           " centroid alignment is frequently used as a pre-processing step for more advanced algorithms."
                           ""
#ifdef DCMA_USE_EIGEN
                           " The 'PCA' option finds an Affine transformation by performing centroid alignment,"
                           " performing principle component analysis (PCA) separately on the reference and moving"
                           " point clouds, computing third-order point distribution moments along each principle axis"
                           " to establish a consistent orientation,"
                           " and then rotates the moving point cloud so the principle axes of the stationary and"
                           " moving point clouds coincide."
                           " The 'PCA' method may be suitable when: (1) both clouds are not contaminated with extra"
                           " noise points (but some Gaussian noise in the form of point 'jitter' should be tolerated)"
                           " and (2) the clouds are not perfectly spherical (i.e., so they have valid principle"
                           " components)."
                           " However, note that the 'PCA' method is susceptible to outliers and can not scale"
                           " a point cloud."
                           " The 'PCA' method will generally fail when the distribution of points shifts across the"
                           " centroid (i.e., comparing reference and moving point clouds) since the orientation of"
                           " the components will be inverted, however 2D degeneracy is handled in a 3D-consistent way,"
                           " and 1D degeneracy is handled in a 1D-consistent way (i.e, the components orthogonal to"
                           " the common line will be completely ambiguous, so spurious rotations will result)."
                           ""
                           " The 'exhaustive_icp' option finds an Affine transformation by first performing PCA-based"
                           " alignment and then iteratively alternating between (1) estimating point-point"
                           " correspondence and (1) solving for a least-squares optimal transformation given this"
                           " correspondence estimate. 'ICP' stands for 'iterative closest point.'"
                           " Each iteration uses the previous transformation *only* to estimate correspondence;"
                           " a least-squares optimal linear transform is estimated afresh each iteration."
                           " The 'exhaustive_icp' method is most suitable when both point clouds consist of"
                           " approximately 50k points or less. Beyond this, ICP will still work but runtime"
                           " scales badly."
                           " ICP is susceptible to outliers and will not scale a point cloud."
                           " It can be used for 2D and 1D degenerate problems, but is not guaranteed to find the"
                           " 'correct' orientation of degenerate or symmetrical point clouds."
#endif
                           "";
    out.args.back().default_val = "centroid";
    out.args.back().expected = true;
#ifdef DCMA_USE_EIGEN
    out.args.back().examples = { "centroid", "pca", "exhaustive_icp" };
#else
    out.args.back().examples = { "centroid" };
#endif


    out.args.emplace_back();
    out.args.back().name = "MaxIterations";
    out.args.back().desc = "If the method is iterative, only permit this many iterations to occur."
                           " Note that this parameter will not have any effect on non-iterative methods.";
    out.args.back().default_val = "100";
    out.args.back().expected = true;
    out.args.back().examples = { "5",
                                 "20",
                                 "100",
                                 "1000" };


    out.args.emplace_back();
    out.args.back().name = "RelativeTolerance";
    out.args.back().desc = "If the method is iterative, terminate the loop when the cost function changes between"
                           " successive iterations by this amount or less."
                           " The magnitude of the cost function will generally depend on the number of points"
                           " (in both point clouds), the scale (i.e., 'width') of the point clouds, the amount"
                           " of noise and outlier points, and any method-specific"
                           " parameters that impact the cost function (if applicable);"
                           " use of this tolerance parameter may be impacted by these characteristics."
                           " Verifying that a given tolerance is of appropriate magnitude is recommended."
                           " Relative tolerance checks can be disabled by setting to non-finite or negative value."
                           " Note that this parameter will not have any effect on non-iterative methods.";
    out.args.back().default_val = "nan";
    out.args.back().expected = true;
    out.args.back().examples = { "-1",
                                 "1E-2",
                                 "1E-3",
                                 "1E-5" };


    out.args.emplace_back();
    out.args.back().name = "Filename";
    out.args.back().desc = "The filename (or full path name) to which the transformation should be written."
                           " Existing files will be overwritten."
                           " The file format is a 4x4 Affine matrix."
                           " If no name is given, a unique name will be chosen automatically.";
    out.args.back().default_val = "";
    out.args.back().expected = true;
    out.args.back().examples = { "transformation.trans",
                                 "trans.txt",
                                 "/path/to/some/trans.txt" };
    out.args.back().mimetype = "text/plain";


    return out;
}



Drover AlignPoints(Drover DICOM_data, OperationArgPkg OptArgs, std::map<std::string,std::string> /*InvocationMetadata*/, std::string FilenameLex){

    Explicator X(FilenameLex);

    //---------------------------------------------- User Parameters --------------------------------------------------
    const auto MovingPointSelectionStr = OptArgs.getValueStr("MovingPointSelection").value();
    const auto ReferencePointSelectionStr = OptArgs.getValueStr("ReferencePointSelection").value();

    const auto MethodStr = OptArgs.getValueStr("Method").value();

    const auto MaxIters = std::stol( OptArgs.getValueStr("MaxIterations").value() );
    const auto RelativeTol = std::stod( OptArgs.getValueStr("RelativeTolerance").value() );

    const auto FilenameStr = OptArgs.getValueStr("Filename").value();

    //-----------------------------------------------------------------------------------------------------------------
    const auto regex_com    = Compile_Regex("^ce?n?t?r?o?i?d?$");
#ifdef DCMA_USE_EIGEN    
    const auto regex_pca    = Compile_Regex("^pc?a?$");
    const auto regex_exhicp = Compile_Regex("^ex?h?a?u?s?t?i?v?e?[-_]?i?c?p?$");
#endif // DCMA_USE_EIGEN

    auto PCs_all = All_PCs( DICOM_data );
    auto ref_PCs = Whitelist( PCs_all, ReferencePointSelectionStr );
    if(ref_PCs.size() != 1){
        throw std::invalid_argument("A single reference point cloud must be selected. Cannot continue.");
    }

    // Iterate over the moving point clouds, aligning each to the reference point cloud.
    auto moving_PCs = Whitelist( PCs_all, MovingPointSelectionStr );
    for(auto & pcp_it : moving_PCs){
        FUNCINFO("There are " << (*pcp_it)->pset.points.size() << " points in the moving point cloud");

        // Determine which filename to use.
        auto FN = FilenameStr;
        if(FN.empty()){
            FN = Get_Unique_Sequential_Filename("/tmp/dcma_alignpoints_", 6, ".trans");
        }
        std::fstream FO(FN, std::fstream::out);

        if(false){
        }else if( std::regex_match(MethodStr, regex_com) ){
            auto t_opt = AlignViaCentroid( (*pcp_it)->pset,
                                           (*ref_PCs.front())->pset );
 
            if(t_opt){
                FUNCINFO("Transforming the point cloud using centre-of-mass alignment");
                t_opt.value().apply_to((*pcp_it)->pset);

                if(!(t_opt.value().write_to(FO))){
                    std::runtime_error("Unable to write transformation to file. Cannot continue.");
                }
            }

#ifdef DCMA_USE_EIGEN    
        }else if( std::regex_match(MethodStr, regex_pca) ){
            auto t_opt = AlignViaPCA( (*pcp_it)->pset,
                                      (*ref_PCs.front())->pset );
 
            if(t_opt){
                FUNCINFO("Transforming the point cloud using principle component alignment");
                t_opt.value().apply_to((*pcp_it)->pset);

                if(!(t_opt.value().write_to(FO))){
                    std::runtime_error("Unable to write transformation to file. Cannot continue.");
                }
            }

        }else if( std::regex_match(MethodStr, regex_exhicp) ){
            auto t_opt = AlignViaExhaustiveICP( (*pcp_it)->pset,
                                                (*ref_PCs.front())->pset,
                                                MaxIters,
                                                RelativeTol );
 
            if(t_opt){
                FUNCINFO("Transforming the point cloud using exhaustive iterative closes point alignment");
                t_opt.value().apply_to((*pcp_it)->pset);

                if(!(t_opt.value().write_to(FO))){
                    std::runtime_error("Unable to write transformation to file. Cannot continue.");
                }
            }
#endif // DCMA_USE_EIGEN

        }else{
            throw std::invalid_argument("Method not understood. Cannot continue.");
        }

    } // Loop over point clouds.

    return DICOM_data;
}
