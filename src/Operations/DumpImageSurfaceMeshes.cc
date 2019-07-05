//DumpImageSurfaceMeshes.cc - A part of DICOMautomaton 2019. Written by hal clark.

#include <asio.hpp>
#include <algorithm>
#include <experimental/optional>
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

#include "../Structs.h"
#include "../Regex_Selectors.h"
#include "../Thread_Pool.h"
#include "DumpImageSurfaceMeshes.h"
#include "Explicator.h"       //Needed for Explicator class.
#include "YgorImages.h"
#include "YgorMath.h"         //Needed for vec3 class.
#include "YgorMisc.h"         //Needed for FUNCINFO, FUNCWARN, FUNCERR macros.
#include "YgorStats.h"        //Needed for Stats:: namespace.
#include "YgorString.h"       //Needed for GetFirstRegex(...)

#include "../Surface_Meshes.h"


OperationDoc OpArgDocDumpImageSurfaceMeshes(void){
    OperationDoc out;
    out.name = "DumpImageSurfaceMeshes";

    out.desc = 
        "This operation generates surface meshes from images and pixel/voxel value thresholds."
        " Output is written to file(s) for viewing with an external viewer (e.g., meshlab)."
        " There are two methods of contour generation available:"
        " a simple binary method in which voxels are either fully in or fully out of the contour,"
        " and a method based on marching cubes that will provide smoother contours."
        " Both methods make use of marching cubes -- the binary method involves pre-processing.";
        
    out.notes.emplace_back(
        "This routine requires images to be regular (i.e., exactly abut nearest adjacent images without"
        " any overlap)."
    );

    out.args.emplace_back();
    out.args.back() = IAWhitelistOpArgDoc();
    out.args.back().name = "ImageSelection";
    out.args.back().default_val = "last";
   

    out.args.emplace_back();
    out.args.back().name = "Lower";
    out.args.back().desc = "The lower bound (inclusive). Pixels with values < this number are excluded from the ROI."
                           " If the number is followed by a '%', the bound will be scaled between the min and max"
                           " pixel values [0-100%]. If the number is followed by 'tile', the bound will be replaced"
                           " with the corresponding percentile [0-100tile]."
                           " Note that upper and lower bounds can be specified separately (e.g., lower bound is a"
                           " percentage, but upper bound is a percentile)."
                           " Note that computed bounds (i.e., percentages and percentiles) consider the entire"
                           " image volume.";
    out.args.back().default_val = "-inf";
    out.args.back().expected = true;
    out.args.back().examples = { "0.0", "-1E-99", "1.23", "0.2%", "23tile", "23.123 tile" };


    out.args.emplace_back();
    out.args.back().name = "Upper";
    out.args.back().desc = "The upper bound (inclusive). Pixels with values > this number are excluded from the ROI."
                           " If the number is followed by a '%', the bound will be scaled between the min and max"
                           " pixel values [0-100%]. If the number is followed by 'tile', the bound will be replaced"
                           " with the corresponding percentile [0-100tile]."
                           " Note that upper and lower bounds can be specified separately (e.g., lower bound is a"
                           " percentage, but upper bound is a percentile)."
                           " Note that computed bounds (i.e., percentages and percentiles) consider the entire"
                           " image volume.";
    out.args.back().default_val = "inf";
    out.args.back().expected = true;
    out.args.back().examples = { "1.0", "1E-99", "2.34", "98.12%", "94tile", "94.123 tile" };


    out.args.emplace_back();
    out.args.back().name = "Channel";
    out.args.back().desc = "The image channel to use. Zero-based.";
    out.args.back().default_val = "0";
    out.args.back().expected = true;
    out.args.back().examples = { "0", "1", "2" };

    
    out.args.emplace_back();
    out.args.back().name = "Method";
    out.args.back().desc = "There are currently two supported methods for generating contours:"
                           " (1) a simple (and fast) binary inclusivity checker, that simply checks if a voxel is within"
                           " the ROI by testing the value at the voxel centre, and (2) a robust (but slow) method based"
                           " on marching cubes. The binary method is fast, but produces extremely jagged contours."
                           " It may also have problems with 'pinches' and topological consistency."
                           " The marching method is more robust and should reliably produce contours for even"
                           " the most complicated topologies, but is considerably slower than the binary method.";
    out.args.back().default_val = "marching";
    out.args.back().expected = true;
    out.args.back().examples = { "binary",
                                 "marching" };

    
    out.args.emplace_back();
    out.args.back().name = "OutBase";
    out.args.back().desc = "The prefix of the filename that surface mesh files will be saved as."
                           " If no name is given, unique names will be chosen automatically.";
    out.args.back().default_val = "";
    out.args.back().expected = true;
    out.args.back().examples = { "/tmp/dicomautomaton_dumpimagesurfacemesh", 
                                 "../somedir/output", 
                                 "/path/to/some/mesh" };

    return out;
}



Drover DumpImageSurfaceMeshes(Drover DICOM_data, OperationArgPkg OptArgs, std::map<std::string,std::string> /*InvocationMetadata*/, std::string FilenameLex){

    //---------------------------------------------- User Parameters --------------------------------------------------
    const auto ImageSelectionStr = OptArgs.getValueStr("ImageSelection").value();
    const auto LowerStr = OptArgs.getValueStr("Lower").value();
    const auto UpperStr = OptArgs.getValueStr("Upper").value();
    const auto ChannelStr = OptArgs.getValueStr("Channel").value();
    const auto MethodStr = OptArgs.getValueStr("Method").value();
    auto OutBase = OptArgs.getValueStr("OutBase").value();

    bool Subdivide = false;
    bool Simplify = false;
    bool Remesh = true;
   
    long int MeshSubdivisions = 2;
    long int RemeshIterations = 5;
    //long int RemeshTargetEdgeLength = 2.5; // DICOM units (mm).
    long int RemeshTargetEdgeLength = 1.5; // DICOM units (mm).
    //long int MeshSimplificationEdgeCountLimit = 75'000; // For later (volumetric) analysis.
    long int MeshSimplificationEdgeCountLimit = 250'000; // For later rendering.

    //-----------------------------------------------------------------------------------------------------------------
    const auto Lower = std::stod( LowerStr );
    const auto Upper = std::stod( UpperStr );
    const auto Channel = std::stol( ChannelStr );

    const auto regex_is_percent = Compile_Regex(".*[%].*");
    const auto Lower_is_Percent = std::regex_match(LowerStr, regex_is_percent);
    const auto Upper_is_Percent = std::regex_match(UpperStr, regex_is_percent);

    const auto regex_is_tile = Compile_Regex(".*p?e?r?c?e?n?tile.*");
    const auto Lower_is_Ptile = std::regex_match(LowerStr, regex_is_tile);
    const auto Upper_is_Ptile = std::regex_match(UpperStr, regex_is_tile);

    const auto binary_regex = Compile_Regex("^bi?n?a?r?y?$");
    const auto marching_regex = Compile_Regex("^ma?r?c?h?i?n?g?$");

    const auto TrueRegex = Compile_Regex("^tr?u?e?$");

    //Iterate over each requested image_array. Each image is processed independently, so a thread pool is used.
    auto IAs_all = All_IAs( DICOM_data );
    auto IAs = Whitelist( IAs_all, ImageSelectionStr );
    for(auto & iap_it : IAs){
        const long int img_count = (*iap_it)->imagecoll.images.size();

        asio_thread_pool tp;
        std::mutex saver_printer; // Who gets to save generated contours, print to the console, and iterate the counter.
        long int completed = 0;

        //Determine the bounds in terms of pixel-value thresholds.
        auto cl = Lower; // Will be replaced if percentages/percentiles requested.
        auto cu = Upper; // Will be replaced if percentages/percentiles requested.
        {
            //Percentage-based.
            if(Lower_is_Percent || Upper_is_Percent){
                Stats::Running_MinMax<float> rmm;
                for(const auto &animg : (*iap_it)->imagecoll.images){
                    animg.apply_to_pixels([&rmm,Channel](long int, long int, long int chnl, float val) -> void {
                         if(Channel == chnl) rmm.Digest(val);
                         return;
                    });
                }
                if(Lower_is_Percent) cl = (rmm.Current_Min() + (rmm.Current_Max() - rmm.Current_Min()) * Lower / 100.0);
                if(Upper_is_Percent) cu = (rmm.Current_Min() + (rmm.Current_Max() - rmm.Current_Min()) * Upper / 100.0);
            }

            //Percentile-based.
            if(Lower_is_Ptile || Upper_is_Ptile){
                std::vector<float> pixel_vals;
                //pixel_vals.reserve(animg.rows * animg.columns * animg.channels * img_count);
                for(const auto &animg : (*iap_it)->imagecoll.images){
                    animg.apply_to_pixels([&pixel_vals,Channel](long int, long int, long int chnl, float val) -> void {
                         if(Channel == chnl) pixel_vals.push_back(val);
                         return;
                    });
                }
                if(Lower_is_Ptile) cl = Stats::Percentile(pixel_vals, Lower / 100.0);
                if(Upper_is_Ptile) cu = Stats::Percentile(pixel_vals, Upper / 100.0);
            }
        }
        if(cl > cu){
            throw std::invalid_argument("Thresholds conflict. Mesh will contain zero faces. Refusing to continue.");
        }

        //Construct a pixel 'oracle' closure using the user-specified threshold criteria. This function identifies whether
        // the pixel is within (true) or outside of (false) the final ROI. This is used for the binary method.
        auto pixel_oracle = [cu,cl](float p) -> bool {
            return (cl <= p) && (p <= cu);
        };

        double inclusion_threshold = std::numeric_limits<double>::quiet_NaN();
        double exterior_value = std::numeric_limits<double>::quiet_NaN();
        bool below_is_interior = false;

        std::list<planar_image<float,double>> masks;
        for(auto &animg : (*iap_it)->imagecoll.images){
            if( (animg.rows < 1) || (animg.columns < 1) || (Channel >= animg.channels) ){
                throw std::runtime_error("Image or channel is empty -- cannot generate surface mesh.");
            }

            //Prepare a mask image for contouring.
            masks.push_back(animg);
            
            if(false){
            }else if(std::regex_match(MethodStr, binary_regex)){
                inclusion_threshold = 0.0;
                below_is_interior = true;
                exterior_value = 1.0;
                const auto interior_value = -exterior_value;
                masks.back().apply_to_pixels([pixel_oracle,
                                              Channel,
                                              interior_value,
                                              exterior_value](long int, 
                                                              long int,
                                                              long int chnl,
                                                              float &val) -> void {
                        if(Channel == chnl){
                            val = (pixel_oracle(val) ? interior_value : exterior_value);
                        }
                        return;
                    });

            }else if(std::regex_match(MethodStr, marching_regex)){
                if(false){
                }else if(std::isfinite(cl) && std::isfinite(cu)){
                    // Transform voxels by their |distance| from the midpoint. Only interior voxels will be within
                    // [0,width*0.5], and all others will be (width*0.5,inf).
                    const double midpoint = (cl + cu) * 0.5;
                    const double width = (cu - cl);

                    inclusion_threshold = width * 0.5;
                    exterior_value = inclusion_threshold + 1.0;
                    below_is_interior = true;
                    masks.back().apply_to_pixels([Channel,midpoint](long int, long int, long int chnl, float &val) -> void {
                            if(Channel == chnl){
                                val = std::abs(val - midpoint);
                            }
                            return;
                        });

                }else if(std::isfinite(cl)){
                    inclusion_threshold = cl;
                    exterior_value = inclusion_threshold - 1.0;
                    below_is_interior = false;

                }else if(std::isfinite(cu)){
                    inclusion_threshold = cu;
                    exterior_value = inclusion_threshold + 1.0;
                    below_is_interior = true;

                }else{ // Neither threshold is finite.
                    throw std::invalid_argument("Unable to discern finite threshold for meshing. Refusing to continue.");
                    // Note: it is possible to deal with these cases and generate meshings for each, i.e., either all
                    // voxels are included or no voxels are included (both are valid meshings). However, it seems
                    // most likely this is a user error. Add this functionality if necessary.

                }

            }else{
                throw std::invalid_argument("Meshing method not recognized. Refusing to continue.");
            }
        }


        // Generate the surface mesh.
        std::list<std::reference_wrapper<planar_image<float,double>>> mask_imgs;
        for(auto &amask : masks){
            mask_imgs.push_back(std::ref(amask));
        }
        auto meshing_params = dcma_surface_meshes::Parameters();
        meshing_params.MutateOpts.inclusivity = Mutate_Voxels_Opts::Inclusivity::Centre;
        meshing_params.MutateOpts.contouroverlap = Mutate_Voxels_Opts::ContourOverlap::Ignore;
        FUNCWARN("Ignoring contour orientations; assuming ROI polyhderon is simple");
        auto output_mesh = dcma_surface_meshes::Estimate_Surface_Mesh_Marching_Cubes( 
                                                        mask_imgs,
                                                        inclusion_threshold, 
                                                        below_is_interior,
                                                        meshing_params );

        // Emit the meshes.
        {
            const auto FN = Get_Unique_Sequential_Filename(OutBase + "_original_mesh_", 6, ".off");
            if(!polyhedron_processing::SaveAsOFF(output_mesh, FN)){
                throw std::runtime_error("Unable to save original mesh as OFF file. Refusing to continue.");
            }
            FUNCINFO("Original mesh written to '" << FN << "'");
        }

        if(Subdivide){
            polyhedron_processing::Subdivide(output_mesh, MeshSubdivisions);
        }
        if(Remesh){
            polyhedron_processing::Remesh(output_mesh, RemeshTargetEdgeLength, RemeshIterations);
        }
        if(Simplify){
            polyhedron_processing::Simplify(output_mesh, MeshSimplificationEdgeCountLimit);
        }

        {
            const auto FN = Get_Unique_Sequential_Filename(OutBase + "_processed_mesh_", 6, ".off");
            if(!polyhedron_processing::SaveAsOFF(output_mesh, FN)){
                throw std::runtime_error("Unable to save processed mesh as OFF file. Refusing to continue.");
            }
            FUNCINFO("Processed mesh written to '" << FN << "'");
        }

    }

    return DICOM_data;
}
