//PETCT_Perfusion_Analysis.cc - A part of DICOMautomaton 2015. Written by hal clark.
//
// This program works with time-series PET-CT perfusion data pulled from a local database,
// supporting a variety of analyses.
//

#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <string>    
#include <vector>
#include <set> 
#include <map>
#include <unordered_map>
#include <list>
#include <functional>
#include <thread>
#include <array>
#include <mutex>
//#include <future>             //Needed for std::async(...)
#include <limits>
#include <cmath>
//#include <cfenv>              //Needed for std::feclearexcept(FE_ALL_EXCEPT).

#include <getopt.h>           //Needed for 'getopts' argument parsing.
#include <cstdlib>            //Needed for exit() calls.
#include <utility>            //Needed for std::pair.
#include <algorithm>
#include <experimental/optional>

#include <boost/algorithm/string.hpp> //For boost:iequals().

#include <pqxx/pqxx>          //PostgreSQL C++ interface.

#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>

#include "Structs.h"
#include "Imebra_Shim.h"      //Wrapper for Imebra library. Black-boxed to speed up compilation.
#include "YgorMisc.h"         //Needed for FUNCINFO, FUNCWARN, FUNCERR macros.
#include "YgorMath.h"         //Needed for vec3 class.
#include "YgorMathPlotting.h" //Needed for YgorMathPlotting::*.
#include "YgorStats.h"        //Needed for Stats:: namespace.
#include "YgorFilesDirs.h"    //Needed for Does_File_Exist_And_Can_Be_Read(...), etc..
#include "YgorContainers.h"   //Needed for bimap class.
#include "YgorPerformance.h"  //Needed for YgorPerformance_dt_from_last().
#include "YgorAlgorithms.h"   //Needed for For_Each_In_Parallel<..>(...)
#include "YgorArguments.h"    //Needed for ArgumentHandler class.
#include "YgorString.h"       //Needed for GetFirstRegex(...)
#include "YgorImages.h"
#include "YgorImagesIO.h"
#include "YgorImagesPlotting.h"

#include "Explicator.h"       //Needed for Explicator class.
#include "Demarcator.h"       //Needed for Demarcator class.

//#include "Distinguisher.h"    //Needed for Distinguisher class.            //Needed?

#include "YgorDICOMTools.h"   //Needed for Is_File_A_DICOM_File(...);

#include "YgorImages_Grouping_Purging_Functors.h"

#include "YgorImages_Processing_Functor_DCEMRI_C_Map.h"
#include "YgorImages_Processing_Functor_DCEMRI_Signal_Difference_C.h"
#include "YgorImages_Processing_Functor_CT_Perfusion_Signal_Diff.h"
#include "YgorImages_Processing_Functor_DCEMRI_AUC_Map.h"
#include "YgorImages_Processing_Functor_DCEMRI_S0_Map.h"
#include "YgorImages_Processing_Functor_DCEMRI_T1_Map.h"
#include "YgorImages_Processing_Functor_DCEMRI_S0_Map_v2.h"
#include "YgorImages_Processing_Functor_DCEMRI_T1_Map_v2.h"
#include "YgorImages_Processing_Functor_Highlight_ROI_Voxels.h"
#include "YgorImages_Processing_Functor_Kitchen_Sink_Analysis.h"
#include "YgorImages_Processing_Functor_Pixel_Value_Histogram.h"
#include "YgorImages_Processing_Functor_IVIMMRI_ADC_Map.h"
#include "YgorImages_Processing_Functor_Time_Course_Slope_Map.h"
#include "YgorImages_Processing_Functor_CT_Perfusion_Clip_Search.h"
#include "YgorImages_Processing_Functor_CT_Perf_Pixel_Filter.h"
#include "YgorImages_Processing_Functor_Min_Pixel_Value.h"
#include "YgorImages_Processing_Functor_Max_Pixel_Value.h"
#include "YgorImages_Processing_Functor_Subtract_Spatially_Overlapping_Images.h"
#include "YgorImages_Processing_Functor_CT_Reasonable_HU_Window.h"
#include "YgorImages_Processing_Functor_Slope_Difference.h"
#include "YgorImages_Processing_Functor_Centralized_Moments.h"
#include "YgorImages_Processing_Functor_Logarithmic_Pixel_Scale.h"
#include "YgorImages_Processing_Functor_per_ROI_Time_Courses.h"
#include "YgorImages_Processing_Functor_DBSCAN_Time_Courses.h"
#include "YgorImages_Processing_Functor_In_Image_Plane_Bilinear_Supersample.h"
#include "YgorImages_Processing_Functor_In_Image_Plane_Bicubic_Supersample.h"
#include "YgorImages_Processing_Functor_Cross_Second_Derivative.h"

#include "YgorImages_Processing_Compute_per_ROI_Time_Courses.h"
#include "YgorImages_Processing_Functor_Liver_Pharmacokinetic_Model.h"

#include "ContourFactories.h"

//Globals. Libimebrashim expects these to be defined.
bool VERBOSE = false;  //Provides additional information.
bool QUIET   = false;  //Suppresses ALL information. Not recommended!


std::unique_ptr<Contour_Data> Combine_Contour_Data(std::unique_ptr<Contour_Data> A,
                                                   std::unique_ptr<Contour_Data> B){
    //This routine simply combines A and B's contour collections. No internal checking is performed.
    // No copying is performed, but A and B are consumed. A is returned as if it were a new pointer.
    A->ccs.splice( A->ccs.end(), std::move(B->ccs) );
    return std::move(A);
}

/*
//A thin wrapper around loaded DICOM data representing a cohesive data set. All data can be shared 
// by default (i.e., copies are shallow by default) so there can be as many 'views' of the data as 
// needed. This class is meant to be stored in a STL container like a std::vector or std::list. If 
// a distinct copy is needed, you should explicitly perform a deep copy of the specific items you 
// need, or of the whole SimpleDriver instance.
//
struct SimpleDrover {
    std::shared_ptr<Contour_Data> contour_data;
    std::shared_ptr<Dose_Array>   dose_data;
    std::shared_ptr<Image_Array>  image_data;


    SimpleDrover(){}

    SimpleDrover(const SimpleDrover &in){
        this->contour_data = in.contour_data;
        this->dose_data    = in.dose_data;
        this->image_data   = in.image_data;
    }

    SimpleDrover(std::shared_ptr<Contour_Data> contours, std::shared_ptr<Dose_Array> dose, std::shared_ptr<Image_Array> imgs){
        this->contour_data = contours;
        this->dose_data    = dose;
        this->image_data   = imgs;
    }

    SimpleDrover DeepCopy(void) const {
        SimpleDrover out;
        out.contour_data = std::make_shared<Contour_Data>();
        *(out.contour_data) = *(this->contour_data);
        out.dose_data = std::make_shared<Dose_Array>();
        *(out.dose_data) = *(this->dose_data);
        out.image_data = std::make_shared<Image_Array>();
        *(out.image_data) = *(this->image_data);
        return out;
    }
};

*/


int main(int argc, char* argv[]){
//---------------------------------------------------------------------------------------------------------------------
//------------------------------------------- Instances used throughout -----------------------------------------------
//---------------------------------------------------------------------------------------------------------------------
    //std::string db_params("dbname=pacs user=hal host=localhost port=63443");
    std::string db_params("dbname=pacs user=hal host=localhost port=5432");

    //These are the means of file input from the database. Each distinct set can be composed of many files which are 
    // executed sequentially in the order provided. Each distinct set can thus create state on the database which can
    // be accessed by later scripts in the set. This facility is provided in case the user needs to run common setup
    // scripts (e.g., to create temporary views, pre-deal with NULLs, setup temporary functions, etc..)
    //
    // Each set is executed separately, and each set produces one distinct image collection. In this way, several image
    // series can be loaded into memory for processing or viewing. 
    std::list<std::list<std::string>> GroupedFilterQueryFiles;
    GroupedFilterQueryFiles.emplace_back();

    //Dump info about the inital data selection and quit without further processing.
    bool OnlyTestQuery = false;

    //Lexicon filename, for Explicator class.
    std::string FilenameLex;

    //The storage place and manager class for loaded image sets, contours, dose matrices, etc..
    Drover DICOM_data;
    

    //Keep note of the FrameofReferenceUIDs we encounter during file loading. Use them to locate any auxiliary contours.
    std::set<std::string> FrameofReferenceUIDs;


    //User-defined tags which are used for helping to keep track of results from computations. Things like how this
    // program was invoked, volunteer tracking numbers, information from the scanning session, etc..
    std::map<std::string,std::string> InvocationMetadata;

    //Operations to perform on the data. (See below for available operations.)
    std::set<std::string> Operations;

//---------------------------------------------------------------------------------------------------------------------
//------------------------------------------------ Option parsing -----------------------------------------------------
//---------------------------------------------------------------------------------------------------------------------
    for(auto i = 0; i < argc; ++i) InvocationMetadata["Invocation"] += std::string(argv[i]) + " ";

    class ArgumentHandler arger;
    const std::string progname(argv[0]);
    arger.examples = { { "--help", 
                         "Show the help screen and some info about the program." },
                       { "-f create_temp_view.sql -f select_records_from_temp_view.sql -o ComputeSomething",
                         "Load a common file and then issue a query which returns something. "
                         "Later files can depend on side effects in the db created by earlier files." },
                       { "-f common.sql -f seriesA.sql -n -f seriesB.sql -o View",
                         "Load two distinct groups of data. The second group does not 'see' the "
                         "file 'common.sql' side effects -- the queries are totally separate." }
                     };
    arger.description = "A program for performing analyses on PET-CT perfusion data.";

    arger.default_callback = [](int, const std::string &optarg) -> void {
      FUNCERR("Unrecognized option with argument: '" << optarg << "'");
      return; 
    };
    arger.optionless_callback = [](const std::string &optarg) -> void {
      FUNCERR("What do you want me to do with the option '" << optarg << "' ?");
      return; 
    };

    arger.push_back( ygor_arg_handlr_t(0, 'T', "test-query-only", false, "", 
      "Print info about first query and quit before processing.",
      [&](const std::string &) -> void {
        OnlyTestQuery = true;
        return;
      })
    );

    arger.push_back( ygor_arg_handlr_t(0, 'l', "lexicon", true, "<best guess>",
      "Lexicon file for normalizing ROI contour names.",
      [&](const std::string &optarg) -> void {
        FilenameLex = optarg;
        return;
      })
    );

    arger.push_back( ygor_arg_handlr_t(1, 'f', "filter-query-file", true, "/tmp/query.sql",
      "Query file(s) to use for filtering which DICOM files should be used for analysis."
      " Files are loaded sequentially and should ultimately return full metadata records.",
      [&](const std::string &optarg) -> void {
        GroupedFilterQueryFiles.back().push_back(optarg);
        return;
      })
    );

    arger.push_back( ygor_arg_handlr_t(2, 'm', "metadata", true, "'Volunteer=01'",
      "Metadata key-value pairs which are tacked onto results destined for a database. "
      "If there is an conflicting key-value pair, the values are concatenated.",
      [&](const std::string &optarg) -> void {
        auto tokens = SplitStringToVector(optarg, '=', 'd');
        if(tokens.size() != 2) FUNCERR("Metadata format not recognized: '" << optarg << "'. Use 'A=B'");
        InvocationMetadata[tokens.front()] += tokens.back();
        return;
      })
    );

    arger.push_back( ygor_arg_handlr_t(3, 'n', "next-group", false, "", 
      "Signifies the beginning of a new (separate from the last) group of filter scripts.",
      [&](const std::string &) -> void {
        GroupedFilterQueryFiles.emplace_back();
        return;
      })
    );

    arger.push_back( ygor_arg_handlr_t(4, 'o', "operation", true, "View",
      "An operation to perform on the fully loaded data. Some operations can be chained, some"
      " may necessarily terminate computation. See source for available operations.",
      [&](const std::string &optarg) -> void {
        Operations.insert(optarg);
        return;
      })
    );

    arger.Launch(argc, argv);

//---------------------------------------------------------------------------------------------------------------------
//------------------------------------------------ Input Verification ---------------------------------------------------
//---------------------------------------------------------------------------------------------------------------------

    //Remove empty groups of query files. Probably not needed, as it ought to get caught at the DB query stage.
    for(auto l_it = GroupedFilterQueryFiles.begin(); l_it != GroupedFilterQueryFiles.end();  ){
        if(l_it->empty()){
            l_it = GroupedFilterQueryFiles.erase(l_it);
        }else{
            ++l_it;
        }
    }
    if(GroupedFilterQueryFiles.empty()){
        FUNCERR("No query files provided. Cannot proceed");
    }
 
    //We require at least one action.
    if(Operations.empty()){
        FUNCWARN("No operations specified: defaulting to operation 'View'");
        Operations.insert("View");
    }

//---------------------------------------------------------------------------------------------------------------------
//----------------------------------------------- Filename Testing ----------------------------------------------------
//---------------------------------------------------------------------------------------------------------------------
    if(FilenameLex.empty()){
        std::list<std::string> trial = { 
                "/home/hal/Dropbox/Project - Explicator/Sample_Lexicons/20150925_SGF_and_SGFQ_tags.lexicon",
                "/home/hal/Dropbox/Project - Explicator/Sample_Lexicons/Frozen/20150925/20150925_SGF_and_SGFQ_tags.lexicon",
                "Lexicons/20150925_SGF_and_SGFQ_tags.lexicon",
                "../Lexicons/20150925_SGF_and_SGFQ_tags.lexicon",
                "20150925_SGF_and_SGFQ_tags.lexicon",
                "/home/hal/Dropbox/Project - DICOMautomaton/Lexicons/20150925_SGF_and_SGFQ_tags.lexicon",
                "/home/hal/20150925_SGF_and_SGFQ_tags.lexicon" };
        for(const auto & f : trial) if(Does_File_Exist_And_Can_Be_Read(f)){
            FilenameLex = f;
            FUNCINFO("No lexicon was explicitly provided. Using file '" << FilenameLex << "' as lexicon");
            break;
        }
        if(FilenameLex.empty()) FUNCERR("Lexicon not located. Please provide one or see program help for more info");
    }


//---------------------------------------------------------------------------------------------------------------------
//----------------------------------------------- Database Initiation -------------------------------------------------
//---------------------------------------------------------------------------------------------------------------------
    FUNCINFO("Executing database queries...");

    //Prepare separate storage space for each of the groups of filter query files. We keep them segregated based on the
    // user's grouping of input query files. This allows us to work on several distinct data sets per invocation, if
    // desired. (Often one just wants to open a single data set, though.)
    typedef decltype(DICOM_data.image_data) loaded_imgs_storage_t;
    std::list<loaded_imgs_storage_t> loaded_imgs_storage;
    typedef decltype(DICOM_data.dose_data) loaded_dose_storage_t;
    std::list<loaded_dose_storage_t> loaded_dose_storage;
    std::shared_ptr<Contour_Data> loaded_contour_data_storage = std::make_shared<Contour_Data>();

    try{
        //Loop over each group of filter query files.
        for(const auto &FilterQueryFiles : GroupedFilterQueryFiles){
            loaded_imgs_storage.emplace_back();
            loaded_dose_storage.emplace_back();

            //Unfortunately, it seems one cannot reset or deactivate/reactivate a connection. So we are forced to 
            // start anew each time.
            //
            // Also note that the libpqxx documentaton states that transactional connections are required if using
            // PostgreSQL large files.
            //
            pqxx::connection c(db_params);
            pqxx::work txn(c);

            //-------------------------------------------------------------------------------------------------------------
            //Query1 stage: select records from the system pacs database.
            //
            //Whatever is in the file(s), let the database figure out if they're legal and valid.
            pqxx::result r1;
     
            std::stringstream ss;
            for(const auto &FilterQueryFile : FilterQueryFiles){ 
                ss << "'" << FilterQueryFile << "'"; //Save the names in case something goes wrong.
                const auto query1 = LoadFileToString(FilterQueryFile);
                r1 = txn.exec(query1);
            }
            if(r1.empty()) FUNCERR("Database query1 stage " << ss.str() << " resulted in no records. Cannot continue");
    
    
            //-------------------------------------------------------------------------------------------------------------
            //Print info about matching records and quit. Useful for figuring out if your query is working or not.
            if(OnlyTestQuery){
                FUNCINFO("Query1 stage: number of records found = " << r1.size());
    
                for(pqxx::result::size_type i = 0; i != r1.size(); ++i){
                    std::cout << "Matching filename = '" << r1[i]["FullPathName"] << "'";
                    std::cout << std::endl;
                }
                continue;
    
            //Otherwise, only dump some basic info if verbosity settings permit.
            }else{
                if(VERBOSE && !QUIET) FUNCINFO("Query1 stage: number of records found = " << r1.size());
            }
    
            //-------------------------------------------------------------------------------------------------------------
            //Query2 stage: process each record, loading whatever data is needed later into memory.
            for(pqxx::result::size_type i = 0; i != r1.size(); ++i){
                FUNCINFO("Parsing file #" << i+1 << "/" << r1.size() << " = " << 100*(i+1)/r1.size() << "%");

                //Get the returned pacsid.
                //const auto pacsid = r1[i]["pacsid"].as<long int>();
                const auto StoreFullPathName = (r1[i]["StoreFullPathName"].is_null()) ? 
                                               "" : r1[i]["StoreFullPathName"].as<std::string>();

                //Parse the file and/or try load the data. Push it into the list (we can collate later).
                // If we cannot ascertain the type then we will treat it as an image and hope it can be loaded.
                const auto Modality = r1[i]["Modality"].as<std::string>();
                if(boost::iequals(Modality,"RTSTRUCT")){
                    const auto preloadcount = loaded_contour_data_storage->ccs.size();
                    try{
                        auto combined = Combine_Contour_Data( std::move(loaded_contour_data_storage->Duplicate()),
                                                              std::move(get_Contour_Data(StoreFullPathName)) );
                        loaded_contour_data_storage = std::move(combined);
                    }catch(const std::exception &e){
                        FUNCWARN("Difficulty encountered during contour data loading: '" << e.what() <<
                                 "'. Ignoring file and continuing");
                        //loaded_contour_data_storage.back().pop_back();
                        continue;
                    }

                    const auto postloadcount = loaded_contour_data_storage->ccs.size();
                    if(postloadcount == preloadcount){
                        FUNCERR("RTSTRUCT file was loaded, but contained no ROIs");
                        //If you get here, it isn't necessarily an error. But something has most likely gone wrong. Why bother
                        // to load an RTSTRUCT file if it is empty? If you know what you're doing, you can safely disable this
                        // error and pop the last-added data. Otherwise, try examining the contour loading code and file data.
                    }


                }else if(boost::iequals(Modality,"RTDOSE")){
                    try{
                        loaded_dose_storage.back().push_back( std::move(Load_Dose_Array(StoreFullPathName)) );
                    }catch(const std::exception &e){
                        FUNCWARN("Difficulty encountered during dose array loading: '" << e.what() <<
                                 "'. Ignoring file and continuing");
                        //loaded_dose_storage.back().pop_back();
                        continue;
                    }

                }else{ //Image loading. 'CT' and 'MR' should work. Not sure about others.
                    try{
                        loaded_imgs_storage.back().push_back( std::move(Load_Image_Array(StoreFullPathName)) );
                    }catch(const std::exception &e){
                        FUNCWARN("Difficulty encountered during image array loading: '" << e.what() <<
                                 "'. Ignoring file and continuing");
                        //loaded_imgs_storage.back().pop_back();
                        continue;
                    }

                    if(loaded_imgs_storage.back().back()->imagecoll.images.size() != 1){
                        FUNCERR("More or less than one image loaded into the image array. You'll need to tweak the code to handle this");
                        //If you get here, you've tried to load a file that contains more than one image slice. This is OK,
                        // (and is legitimate behaviour) but you'll need to update the following code to ensure each file's 
                        // metadata is set accordingly. This is all you need to do at the time of writing, but take a look over
                        // the rest of the code to ensure the code doesn't assume too much.
                    }
 
                    //If we want to add any additional image metadata, or replace the default Imebra_Shim.cc populated metadata
                    // with, say, the non-null PostgreSQL metadata, it should be done here.
                    if(!r1[i]["dt"].is_null()){
                        //DICOM_data.image_data.back()->imagecoll.images.back().metadata["dt"] = r1[i]["dt"].c_str();
                        //loaded_imgs_storage.back()->imagecoll.images.back().metadata["dt"] = r1[i]["dt"].c_str();
                        loaded_imgs_storage.back().back()->imagecoll.images.back().metadata["dt"] = r1[i]["dt"].c_str();
                    }
                    // ... more metadata operations ...
                }

                //Whatever file type, 
                if(!r1[i]["FrameofReferenceUID"].is_null()){
                    FrameofReferenceUIDs.insert(r1[i]["FrameofReferenceUID"].as<std::string>());
                }

            }

            //Double-check before proceeding that we aren't going to accidentally commit something we don't want to.
            if(OnlyTestQuery) FUNCERR("Programming error. Test queries should never reach this point!");

            //-------------------------------------------------------------------------------------------------------------
            //Finish the transaction and drop the connection.
            txn.commit();

        } // Loop over groups of query filter files.

    }catch(const std::exception &e){
        FUNCERR("Exception caught: " << e.what());
    }

    //If only testing the queries, die before committing the transaction.
    if(OnlyTestQuery) return 0;


    //Custom contour loading from an auxiliary database.
    if(!FrameofReferenceUIDs.empty()){
        try{
            pqxx::connection c(db_params);
            pqxx::work txn(c);

            //Query for any contours matching the specific FrameofReferenceUID.
            std::stringstream ss;
            ss << "SELECT * FROM contours WHERE ";
            {
                bool first = true;
                for(const auto &FrameofReferenceUID : FrameofReferenceUIDs){
                    if(first){
                        first = false;
                        ss << "(FrameofReferenceUID = " << txn.quote(FrameofReferenceUID) << ") ";
                    }else{
                        ss << "OR (FrameofReferenceUID = " << txn.quote(FrameofReferenceUID) << ") ";
                    }
                }
            }
            ss << ";";
            pqxx::result res = txn.exec(ss.str());

            //Parse any matching contour collections. Store them for later. 
            for(pqxx::result::size_type i = 0; i != res.size(); ++i){
                const auto ROIName                 = res[i]["ROIName"].as<std::string>();
                const auto ContourCollectionString = res[i]["ContourCollectionString"].as<std::string>();
                const auto StudyInstanceUID        = res[i]["StudyInstanceUID"].as<std::string>();
                const auto FrameofReferenceUID     = res[i]["FrameofReferenceUID"].as<std::string>();
       
                 
                const auto keyA = std::make_pair(FrameofReferenceUID,StudyInstanceUID);
                contours_with_meta cc;
                if(!cc.load_from_string(ContourCollectionString)){
                    FUNCWARN("Unable to parse contour collection with ROIName '" << ROIName <<
                             "' and StudyInstanceUID '" << StudyInstanceUID << "'. Continuing");
                    continue;
                }else{
                    FUNCINFO("Loaded contour with StudyInstanceUID '" << StudyInstanceUID << "' and ROIName '" << ROIName << "'");

                    //Imbue the contours with their names and any other relevant metadata.
                    for(auto & contour : cc.contours){
                        contour.metadata["ROIName"] = ROIName;
                        contour.metadata["StudyInstanceUID"] = StudyInstanceUID;
                        contour.metadata["FrameofReferenceUID"] = FrameofReferenceUID;
                        // ...
                    }

                    // ---- Unmodified contours ----
                    //Pack into the group's existing contour collection.
                    loaded_contour_data_storage->ccs.emplace_back(cc);

                    /*
                    // ---- Modified contours ----
                    {
                      const auto CCCentroid = cc.Centroid();
                      const plane<double> slightlyaskewplane(vec3<double>(0.0,1.0,0.0), CCCentroid);
                      auto split_ccs = cc.Split_Along_Plane(slightlyaskewplane);
                     
                      for(auto & contour : split_ccs.front().contours){
                          contour.metadata["ROIName"] += "_POST"; 
                      }
                      for(auto & contour : split_ccs.back().contours){
                          contour.metadata["ROIName"] += "_ANT"; 
                      }
               
                      loaded_contour_data_storage->ccs.emplace_back(split_ccs.front());
                      loaded_contour_data_storage->ccs.emplace_back(split_ccs.back());
                    } */
                }
            }
        
            //No transaction needed. Read-only.
            //txn.commit();
        }catch(const std::exception &e){
            FUNCWARN("Unable to select contours: exception caught: " << e.what());
        }
    } //Loading custom contours from an auxiliary database.

    //Pack the data into a Drover instance.
    DICOM_data.contour_data = loaded_contour_data_storage;

    //Attempt contour name normalization using the selected lexicon.
    {
        Explicator X(FilenameLex);
        for(auto & cc : DICOM_data.contour_data->ccs){
             for(auto & c : cc.contours){
                 const auto NormalizedROIName = X(c.metadata["ROIName"]); //Could be cached, externally or internally.
                 c.metadata["NormalizedROIName"] = NormalizedROIName;
             }
        }   
    }

    //Stuff references to all contours into a list. Remember that you can still address specific contours through
    // the original holding containers (which are not modified here).
    std::list<std::reference_wrapper<contour_collection<double>>> cc_all;
    for(auto & cc : DICOM_data.contour_data->ccs){
        auto base_ptr = reinterpret_cast<contour_collection<double> *>(&cc);
        cc_all.push_back( std::ref(*base_ptr) );
    }


    //Collate each group of images into a single set, if possible. Also stuff the correct contour data in the same set.
    // Also load dose data into the fray.
    for(auto & loaded_img_set : loaded_imgs_storage){
        if(loaded_img_set.empty()) continue;

        auto collated_imgs = Collate_Image_Arrays(loaded_img_set);
        if(!collated_imgs) FUNCERR("Unable to collate images. It is possible to continue, but only if you are able to handle this case");

        DICOM_data.image_data.emplace_back(std::move(collated_imgs));
    }
    if(VERBOSE && !QUIET) FUNCINFO("Number of image set groups loaded = " << DICOM_data.image_data.size());

    for(auto & loaded_dose_set : loaded_dose_storage){
        if(loaded_dose_set.empty()) continue;

        //Stuff the dose data into the Drover's Dose_Array.
        //DICOM_data.dose_data.emplace_back( std::move(loaded_dose_set.back()) );

        //Stuff the dose data into the Drover's Image_Array so it can be more easily used with image processing routines.
        DICOM_data.image_data.emplace_back();
        DICOM_data.image_data.back() = std::make_shared<Image_Array>(*(loaded_dose_set.back()));
    }
    //if(!DICOM_data.Has_Image_Data()) FUNCERR("No images available for processing. Cannot continue"); //Disable if needed.
    if(!DICOM_data.Has_Image_Data()) FUNCWARN("No images available for processing. You may encounter difficulties!");

    //Explicitly sort images within an image collection, instead of relying on the SQL filter's group ordering.
    if(false){
        for(auto & img_array : DICOM_data.image_data){
            img_array->imagecoll.Stable_Sort_on_Metadata_Keys_Value_Numeric<long int>("InstanceNumber");
            img_array->imagecoll.Stable_Sort_on_Metadata_Keys_Value_Numeric<double>("SliceLocation");
            img_array->imagecoll.Stable_Sort_on_Metadata_Keys_Value_Lexicographic("Modality");
        }
    }

    //-----------------------------------------------------------------------------------------------------------------
    //Begin analysis.

    //Dump exactly what order the data will be in for the following analysis.
    const auto Dump_All_Ordered_Image_Metadata_To_File = 
        [](const decltype(DICOM_data.image_data.front()->imagecoll.images) &images, const std::string &dumpfile) -> void {

            //Get a superset of all metadata names.
            std::set<std::string> sset;
            for(const auto &img : images){
                for(const auto &md_pair : img.metadata){
                    sset.insert(md_pair.first);
                }
            }

            //Cycle through the images and print available tags.
            std::stringstream df;
            for(const auto &akey : sset) df << akey << "\t";
            df << std::endl;
            for(const auto &img : images){
                for(const auto &akey : sset) df << img.metadata.find(akey)->second << "\t";
                df << std::endl;
            }
            if(!OverwriteStringToFile(df.str(),dumpfile)) FUNCERR("Unable to dump ordered image metadata to file");
            return;
        };
    if(false){
        Dump_All_Ordered_Image_Metadata_To_File(DICOM_data.image_data.front()->imagecoll.images, 
                                                "/tmp/ordered_image_metadata.tsv");
    }

    //Dump all the metadata elements, but group like-items together and also print the occurence number.
    const auto Dump_Image_Metadata_Occurrences_To_File =
       // [](const decltype(DICOM_data.image_data.front()->imagecoll.images) &images, const std::string &dumpfile) -> void {
        [](const std::list<planar_image<float,double>> &images, const std::string &dumpfile) -> void {

            //Get a superset of all metadata names. Also bump the count for each metadata item.
            std::map<std::string, std::map<std::string,long int>> sset;
            for(const auto &img : images){
                for(const auto &md_pair : img.metadata){
                    ++(sset[md_pair.first][md_pair.second]);
                }
            }

            //Get the maximum unique map length.
            size_t maxm = 0;
            for(const auto &sspair : sset) if(sspair.second.size() > maxm) maxm = sspair.second.size();
 
            //Cycle through the images and print available tags.
            std::stringstream df;
            for(const auto &sspair : sset) df << sspair.first << "\tcount\t";
            df << std::endl;

            for(size_t i = 0; i < maxm; ++i){
                for(const auto &sspair : sset){
                    if(i < sspair.second.size()){
                        const auto pit = std::next(sspair.second.begin(),i);
                        df << pit->first << "\t" << pit->second << "\t";
                    }else{
                        df << "\t\t";
                    }
                }
                df << std::endl;
            }
            if(!OverwriteStringToFile(df.str(),dumpfile)) FUNCERR("Unable to dump ordered image metadata to file");
            return;
        };
    if(false){
        size_t i = 0;
        for(auto & img_array : DICOM_data.image_data){
            std::string fname = "/tmp/petct_analysis_img_array_metadata_occurences_"_s + Xtostring(i) + ".tsv";
            Dump_Image_Metadata_Occurrences_To_File(img_array->imagecoll.images, fname);
            ++i;
        }
        return 0;
    }


    //Grab an arbitrary point from one of the images. Find all other images which encompass the point.
    if(false){
        const auto apoint = DICOM_data.image_data.front()->imagecoll.images.front().center();
        auto encompassing_images = DICOM_data.image_data.front()->imagecoll.get_images_which_encompass_point(apoint);

        FUNCINFO("Found " << encompassing_images.size() << " images which encompass the point " << apoint);
    }

    //Output the pixel values over time for a generic point.
    if(false){
        const auto apoint = DICOM_data.image_data.front()->imagecoll.images.front().center();
        auto encompassing_images = DICOM_data.image_data.front()->imagecoll.get_images_which_encompass_point(apoint);
        const int channel = 0;

        std::cout << "time\t";
        std::cout << "pixel intensity\t";
        std::cout << "modality\t";
        std::cout << "image center\t";
        std::cout << "image volume" << std::endl;
        for(const auto &img_it : encompassing_images){
            std::cout << img_it->metadata.find("FrameReferenceTime")->second << "\t";
            std::cout << img_it->value(apoint, channel) << "\t";
            std::cout << img_it->metadata.find("Modality")->second << "\t";
            std::cout << img_it->center() << "\t";
            std::cout << (img_it->rows * img_it->columns * img_it->pxl_dx * img_it->pxl_dy * img_it->pxl_dz) << std::endl;
        }
    }

    //Process or transform images to produce hybrid images, parameter maps (DCE-MRI analysis; S0 and T1 maps, C(t) images),
    // and other things like histograms or time courses.
        

    //=================================================================================================================
    //========================================== Pre-Analysis Processing ==============================================
    //=================================================================================================================
    if(Operations.count("PreFilterEnormousCTValues") != 0){
        //This operation runs the data through a per-pixel filter, censoring pixels which are too high to legitimately
        // show up in a clinical CT. Censored pixels are set to zero. Data is modified and no copy is made!
        for(auto & img_arr : DICOM_data.image_data){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   CTPerfEnormousPixelFilter,
                                                   { } )){
                FUNCERR("Unable to censor pixels with enormous values");
            }
        }
    }

    if(Operations.count("GiveWholeImageArrayAHeadAndNeckWindowLevel") != 0){
        //This operation runs the images in an image array through a uniform window-and-leveler instead of per-slice
        // window-and-level or no window-and-level at all. Data is modified and no copy is made!
        for(auto & img_arr : DICOM_data.image_data){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   StandardHeadAndNeckHUWindow,
                                                   { } )){
                FUNCERR("Unable to force window to cover a reasonable head-and-neck HU range");
            }
        }
    }


    if(Operations.count("GiveWholeImageArrayAnAbdominalWindowLevel") != 0){
        //This operation runs the images in an image array through a uniform window-and-leveler instead of per-slice
        // window-and-level or no window-and-level at all. Data is modified and no copy is made!
        for(auto & img_arr : DICOM_data.image_data){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   StandardAbdominalHUWindow,
                                                   { } )){
                FUNCERR("Unable to force window to cover a reasonable abdominal HU range");
            }
        }
    }

    if(Operations.count("GiveWholeImageArrayAThoraxWindowLevel") != 0){
        //This operation runs the images in an image array through a uniform window-and-leveler instead of per-slice
        // window-and-level or no window-and-level at all. Data is modified and no copy is made!
        for(auto & img_arr : DICOM_data.image_data){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   StandardThoraxHUWindow,
                                                   { } )){
                FUNCERR("Unable to force window to cover a reasonable thorax HU range");
            }
        }
    }


    if(Operations.count("GiveWholeImageArrayABoneWindowLevel") != 0){
        //This operation runs the images in an image array through a uniform window-and-leveler instead of per-slice
        // window-and-level or no window-and-level at all. Data is modified and no copy is made!
        for(auto & img_arr : DICOM_data.image_data){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   StandardBoneHUWindow,
                                                   { } )){
                FUNCERR("Unable to force window to cover a reasonable bone HU range");
            }
        }
    }


    //=================================================================================================================
    //============================================= Contour Operations ================================================
    //=================================================================================================================
    if(Operations.count("DumpROIData") != 0){
        //Simply dump ROI contour information to stdout.
        typedef std::tuple<std::string,std::string,std::string> key_t; //PatientID, ROIName, NormalizedROIName.

        std::map<key_t,int> NameCounts;
        if(DICOM_data.contour_data != nullptr){
            for(auto & cc : DICOM_data.contour_data->ccs){
                for(auto & c : cc.contours){
                    key_t key = std::tie(c.metadata["PatientID"], c.metadata["ROIName"], c.metadata["NormalizedROIName"]);
                    NameCounts[key] += 1;
                }
            }
        }

        Explicator X(FilenameLex);
        for(auto & NameCount : NameCounts){
            //Simply dump the suspected mapping.
            //std::cout << "PatientID='" << std::get<0>(NameCount.first) << "'\t"
            //          << "ROIName='" << std::get<1>(NameCount.first) << "'\t"
            //          << "NormalizedROIName='" << std::get<2>(NameCount.first) << "'\t"
            //          << "Contours='" << NameCount.second << "'"
            //          << std::endl;

            //Print out the best few guesses for each raw contour name.
            const auto ROIName = std::get<1>(NameCount.first);
            X(ROIName);
            std::unique_ptr<std::map<std::string,float>> res(std::move(X.Get_Last_Results()));
            std::vector<std::pair<std::string,float>> ordered_res(res->begin(), res->end());
            std::sort(ordered_res.begin(), ordered_res.end(), 
                      [](const std::pair<std::string,float> &L, const std::pair<std::string,float> &R) -> bool {
                          return L.second > R.second;
                      });
            if(ordered_res.size() != 1) for(size_t i = 0; i < ordered_res.size(); ++i){
                std::cout << ordered_res[i].first << " : " << ROIName; // << std::endl;
                //std::cout << " QQQ " << ordered_res[i].second << std::endl;
                std::cout << std::endl;
                //if(i > 1) break;
            }
        }
        std::cout << std::endl;
    }

    //=================================================================================================================
    //============================================= UBC3TMRI TD03 IVIM ================================================
    //=================================================================================================================
    if(Operations.count("UBC3TMRI_IVIM_ADC") != 0){
        //Get handles for each of the original image arrays so we can easily refer to them later.
        std::vector<std::shared_ptr<Image_Array>> orig_img_arrays;
        for(auto & img_arr : DICOM_data.image_data) orig_img_arrays.push_back(img_arr);

        //std::shared_ptr<Image_Array> img_arr_orig_series_501 = *std::next(DICOM_data.image_data.begin(),0);
        //std::shared_ptr<Image_Array> img_arr_orig_series_601 = *std::next(DICOM_data.image_data.begin(),1);
   
        //Deep-copy and compute an ADC map using the various images with varying diffusion b-values.
        // This will leave us with a time series of ADC parameters (the 1DYN series will have a single time point,
        // but the 5DYN series will have five time points).
        std::vector<std::shared_ptr<Image_Array>> adc_map_img_arrays;
        for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            adc_map_img_arrays.emplace_back( DICOM_data.image_data.back() );
            //std::shared_ptr<Image_Array> img_arr_series_501_ADC_map( DICOM_data.image_data.back() );

            if(!adc_map_img_arrays.back()->imagecoll.Process_Images( GroupSpatiallyTemporallyOverlappingImages, 
                                                                     IVIMMRIADCMap, 
                                                                     { } )){
                FUNCERR("Unable to generate ADC map");
            }
        }



        //Deep-copy the ADC map and compute a slope-sign map.
        std::vector<std::shared_ptr<Image_Array>> slope_sign_map_img_arrays;
        auto TimeCourseSlopeMapAllTime = std::bind(TimeCourseSlopeMap, 
                                                   std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, 
                                                   std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
                                                   std::experimental::any());
        for(auto & img_arr : adc_map_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            slope_sign_map_img_arrays.emplace_back( DICOM_data.image_data.back() );
            //std::shared_ptr<Image_Array> img_arr_time_course_slope_map( DICOM_data.image_data.back() );

            if(!slope_sign_map_img_arrays.back()->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                                            TimeCourseSlopeMapAllTime,
                                                                            { } )){
                FUNCERR("Unable to compute time course slope map");
            }
        }

    }


    //=================================================================================================================
    //========================================== BCCA CT Perfusion Liver ==============================================
    //=================================================================================================================
    if(Operations.count("CT_Liver_Perfusion_First_Run") != 0){
        //Use this mode when looking at the data for the first time. It avoids computing much,
        // just lets you *look* at the data, find t_0, etc..

        //Get handles for each of the original image arrays so we can easily refer to them later.
        std::vector<std::shared_ptr<Image_Array>> orig_img_arrays;
        for(auto & img_arr : DICOM_data.image_data) orig_img_arrays.push_back(img_arr);

        //Force the window to something reasonable to be uniform and cover normal tissue HU range.
        if(true) for(auto & img_arr : orig_img_arrays){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   StandardAbdominalHUWindow,
                                                   { } )){
                FUNCERR("Unable to force window to cover reasonable HU range");
            }
        }

        ////Attempt to filter out (apparently) non-physical pixels by setting them to zero.
        //// This helps when the data may be scaled strangely.
        //std::vector<std::shared_ptr<Image_Array>> enormous_pixel_censored_img_arrays;
        //for(auto & img_arr : orig_img_arrays){
        //    DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
        //    enormous_pixel_censored_img_arrays.emplace_back( DICOM_data.image_data.back() );
        //
        //    if(!enormous_pixel_censored_img_arrays.back()->imagecoll.Process_Images( GroupIndividualImages,
        //                                                                             CTPerfEnormousPixelFilter,
        //                                                                             { } )){
        //        FUNCERR("Unable to censor pixels with enormous values");
        //    }
        //}

        //Temporally average the images.
        std::vector<std::shared_ptr<Image_Array>> temp_avgd;
        if(true) for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            temp_avgd.emplace_back( DICOM_data.image_data.back() );

            if(!temp_avgd.back()->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)){
                FUNCERR("Cannot temporally average images");
            }
        }

        //Force the window to something reasonable to be uniform and cover normal tissue HU range.
        if(true) for(auto & img_arr : temp_avgd){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   StandardAbdominalHUWindow,
                                                   { } )){
                FUNCERR("Unable to force window to cover reasonable HU range");
            }
        }


        //Construct perpendicular image slices that align with first row and column of the first image.
        std::vector<std::shared_ptr<Image_Array>> intersecting_row;
        //std::vector<std::shared_ptr<Image_Array>> intersecting_col;
        if(true) for(auto & img_arr : temp_avgd){
            //Generate blank image arrays for the output.
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( ) );
            intersecting_row.emplace_back( DICOM_data.image_data.back() );
            //DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( ) );
            //intersecting_col.emplace_back( DICOM_data.image_data.back() );

            //Figure out outgoing image spatial attributes.
            auto first_img_it = img_arr->imagecoll.images.begin();
            const auto old_row_unit = first_img_it->row_unit;
            const auto old_col_unit = first_img_it->col_unit;
            const auto old_orto_unit = old_row_unit.Cross( old_col_unit );

            const auto new_row_unit = old_orto_unit;
            const auto new_col_unit = old_row_unit; //Chosen to new_row x new_col = old_col
            const auto new_ortho_unit = old_col_unit;
                                                                               
            const auto L = std::abs( (img_arr->imagecoll.images.back().offset 
                                     - img_arr->imagecoll.images.front().offset).Dot( old_orto_unit ) ); 
                             // ^^^ ASSUMES SORTED ORDER! REPLACE WITH minmix_z() or use:
                             //         planar_image_collection::Stable_Sort(std::function<bool(const planar_image<T,R> &lhs, const planar_image<T,R> &rhs)> lt_func);
            const auto N = L / first_img_it->pxl_dz; // The number needed to make a continuous image with the same pixel thickness.

            const auto numb_of_imgs = first_img_it->columns;
            const auto numb_of_rows = 1*static_cast<long int>( std::ceil(N) );
            const auto numb_of_cols = first_img_it->rows;
            const auto numb_of_chns = first_img_it->channels;
            const auto new_pxl_dx = first_img_it->pxl_dz / 1.0;
            const auto new_pxl_dy = first_img_it->pxl_dx;
            const auto new_pxl_dz = first_img_it->pxl_dy;

            const auto anchor = first_img_it->anchor;
            const auto offset = first_img_it->offset;

            for(auto i = 0; i < numb_of_imgs; ++i){
                intersecting_row.back()->imagecoll.images.emplace_back();
                intersecting_row.back()->imagecoll.images.back().init_buffer( numb_of_rows, numb_of_cols, numb_of_chns );
                intersecting_row.back()->imagecoll.images.back().init_spatial( new_pxl_dx, new_pxl_dy, new_pxl_dz, 
                                                                               anchor, offset + new_ortho_unit * new_pxl_dz * i );
                intersecting_row.back()->imagecoll.images.back().init_orientation( new_row_unit, new_col_unit );
 
                intersecting_row.back()->imagecoll.images.back().fill_pixels(std::numeric_limits<float>::quiet_NaN());
              
                const auto count = img_arr->imagecoll.Intersection_Copy( intersecting_row.back()->imagecoll.images.back() );
                if(count == 0) FUNCWARN("Produced image with zero intersections. Bounds were not specified properly."
                                        " This is not an error, but a wasteful extra image has been created");

                intersecting_row.back()->imagecoll.images.back().metadata["Rows"] = std::to_string(numb_of_rows);
                intersecting_row.back()->imagecoll.images.back().metadata["Columns"] = std::to_string(numb_of_cols);
                intersecting_row.back()->imagecoll.images.back().metadata["PixelSpacing"] = std::to_string(new_pxl_dx) 
                                                                                            + "^" + std::to_string(new_pxl_dy);
                intersecting_row.back()->imagecoll.images.back().metadata["SliceThickness"] = std::to_string(new_pxl_dz);
                intersecting_row.back()->imagecoll.images.back().metadata["Description"] = "Volume Intersection: Row";

            }
        }

        //Force the window to something reasonable to be uniform and cover normal tissue HU range.
        if(true) for(auto & img_arr : intersecting_row){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   StandardAbdominalHUWindow,
                                                   { } )){
                FUNCERR("Unable to force window to cover reasonable HU range");
            }
        }

        //Average all images together.
        std::vector<std::shared_ptr<Image_Array>> all_avgd;
        if(true) for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            all_avgd.emplace_back( DICOM_data.image_data.back() );

            if(!all_avgd.back()->imagecoll.Process_Images( GroupAllImages,
                                                           CondenseMaxPixel,
                                                           { } )){
                FUNCERR("Unable to generate min(pixel) images");
            }
        }

    }

    if(Operations.count("CT_Liver_Perfusion_Pharmaco") != 0){
        //Get handles for each of the original image arrays so we can easily refer to them later.
        std::vector<std::shared_ptr<Image_Array>> orig_img_arrays;
        for(auto & img_arr : DICOM_data.image_data) orig_img_arrays.push_back(img_arr);


        //Force the window to something reasonable to be uniform and cover normal tissue HU range.
        if(true) for(auto & img_arr : orig_img_arrays){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   StandardAbdominalHUWindow,
                                                   { } )){
                FUNCERR("Unable to force window to cover reasonable HU range");
            }
        }


        //Compute a baseline with which we can use later to compute signal enhancement.
        std::vector<std::shared_ptr<Image_Array>> baseline_img_arrays;
        if(false){
            //Baseline = temporally averaged pre-contrast-injection signal.

            double ContrastInjectionLeadTime = 10.0; //Seconds. 
            if(InvocationMetadata.count("ContrastLeadTime") == 0){
                FUNCWARN("Unable to locate 'ContrastLeadTime' invocation metadata key. Assuming the default lead time "
                         << ContrastInjectionLeadTime << "s is appropriate");
            }else{
                ContrastInjectionLeadTime = std::stod( InvocationMetadata["ContrastLeadTime"] );
                if(ContrastInjectionLeadTime < 0.0) throw std::runtime_error("Non-sensical 'ContrastLeadTime' found.");
                FUNCINFO("Found 'ContrastLeadTime' invocation metadata key. Using value " << ContrastInjectionLeadTime << "s");
            }
            auto PurgeAboveNSeconds = std::bind(PurgeAboveTemporalThreshold, std::placeholders::_1, ContrastInjectionLeadTime);

            for(auto & img_arr : orig_img_arrays){
                DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
                baseline_img_arrays.emplace_back( DICOM_data.image_data.back() );
    
                baseline_img_arrays.back()->imagecoll.Prune_Images_Satisfying(PurgeAboveNSeconds);
    
                if(!baseline_img_arrays.back()->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)){
                    FUNCERR("Cannot temporally average data set. Is it able to be averaged?");
                }
            }
    
        }else{
            //Baseline = minimum of signal over whole time course (minimum is usually pre-contrast, but noise
            // can affect the result).

            for(auto & img_arr : orig_img_arrays){
                DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
                baseline_img_arrays.emplace_back( DICOM_data.image_data.back() );
    
                if(!baseline_img_arrays.back()->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                                          CondenseMinPixel,
                                                                          { } )){
                    FUNCERR("Unable to generate min(pixel) images over the time course");
                }
            }
        }


        //Deep-copy the original long image array and use the baseline map to work out 
        // approximate contrast enhancement in each voxel.
        std::vector<std::shared_ptr<Image_Array>> C_enhancement_img_arrays;
        {
            auto img_arr = orig_img_arrays.front();
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            C_enhancement_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!C_enhancement_img_arrays.back()->imagecoll.Transform_Images( CTPerfusionSigDiffC,
                                                                             { baseline_img_arrays.front()->imagecoll },
                                                                             { } )){
                FUNCERR("Unable to transform image array to make poor-man's C map");
            }
        }
       
        //Eliminate the original, un-processed data to relieve some memory pressure.
        if(true){
            auto PurgeAboveNSeconds = std::bind(PurgeAboveTemporalThreshold, std::placeholders::_1, 20.0);
            for(auto & img_arr : orig_img_arrays){
                img_arr->imagecoll.Prune_Images_Satisfying(PurgeAboveNSeconds);
            }
        }
 
        //Compute some aggregate C(t) curves from the available ROIs. We especially want the 
        // portal vein and ascending aorta curves.
        ComputePerROITimeCoursesUserData ud; // User Data.
        for(auto & img_arr : C_enhancement_img_arrays){
            if(!img_arr->imagecoll.Compute_Images( ComputePerROICourses,   //Non-modifying function, can use in-place.
                                                   { },
                                                   cc_all,
                                                   &ud )){
                FUNCERR("Unable to compute per-ROI time courses");
            }
        }

        if(false){

            // TODO: This little plotting routine is not working. Have I tweaked something I shouldn't have?

            std::cout << "Produced " << ud.time_courses.size() << " time courses:" << std::endl;

            std::vector<YgorMathPlotting::Shuttle<samples_1D<double>>> shuttle;
            for(auto & tcs : ud.time_courses){
                const auto lROIname = tcs.first;
                const auto lVoxelCount = ud.voxel_count[lROIname];
                const auto lTimeCourse = tcs.second.Multiply_With(1.0/static_cast<double>(lVoxelCount));
                shuttle.emplace_back(lTimeCourse, lROIname + " - Voxel Averaged");
                std::cout << "\t'" << lROIname << "'" << std::endl; 

                lTimeCourse.Write_To_File(Get_Unique_Sequential_Filename("/tmp/roi_time_course_",4,".txt")); 
            }
            YgorMathPlotting::Plot<double>(shuttle, "ROI Time Courses", "Time (s)", "Pixel Intensity");

            FUNCINFO("Waiting for you to press enter..");
            double goon;
            std::cin >> goon;
        }

        //Prune some images, to reduce the computational effort needed.
        for(auto & img_arr : C_enhancement_img_arrays){
            const auto centre = img_arr->imagecoll.center();
            img_arr->imagecoll.Retain_Images_Satisfying(
                                  [=](const planar_image<float,double> &animg)->bool{
                                      return animg.encompasses_point(centre);
                                  });
        }


        //Using the ROI time curves, compute a pharmacokinetic model and produce an image map with some model parameter(s).
        std::vector<std::shared_ptr<Image_Array>> pharmaco_model_arr;
        for(auto & img_arr : C_enhancement_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            pharmaco_model_arr.emplace_back( DICOM_data.image_data.back() );

            if(!pharmaco_model_arr.back()->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                                     LiverPharmacoModel,
                                                                     cc_all,
                                                                     &ud )){
                FUNCERR("Unable to pharmacokinetically model liver!");
            }
        }

    }


    if(Operations.count("CT_Liver_Perfusion") != 0){
        //Get handles for each of the original image arrays so we can easily refer to them later.
        std::vector<std::shared_ptr<Image_Array>> orig_img_arrays;
        for(auto & img_arr : DICOM_data.image_data) orig_img_arrays.push_back(img_arr);


        ////Force the window to something reasonable to cover normal tissue HU range. (For viewing purposes only!)
        //std::vector<std::shared_ptr<Image_Array>> window_forced_img_arrays;
        //if(false) for(auto & img_arr : orig_img_arrays){
        //    DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
        //    window_forced_img_arrays.emplace_back( DICOM_data.image_data.back() );
        //
        //    if(!window_forced_img_arrays.back()->imagecoll.Process_Images( GroupIndividualImages,
        //                                                                   StandardAbdominalHUWindow,
        //                                                                   { } )){
        //        FUNCERR("Unable to force window to cover reasonable HU range");
        //    }
        //}

        //Force the window to something reasonable to be uniform and cover normal tissue HU range.
        if(true) for(auto & img_arr : orig_img_arrays){
            if(!img_arr->imagecoll.Process_Images( GroupIndividualImages,
                                                   StandardAbdominalHUWindow,
                                                   { } )){
                FUNCERR("Unable to force window to cover reasonable HU range");
            }
        }


        //Compute a baseline with which we can use later to compute signal enhancement.
        std::vector<std::shared_ptr<Image_Array>> baseline_img_arrays;

        if(false){
            //Baseline = temporally averaged pre-contrast-injection signal.

            double ContrastInjectionLeadTime = 10.0; //Seconds. 
            if(InvocationMetadata.count("ContrastLeadTime") == 0){
                FUNCWARN("Unable to locate 'ContrastLeadTime' invocation metadata key. Assuming the default lead time "
                         << ContrastInjectionLeadTime << "s is appropriate");
            }else{
                ContrastInjectionLeadTime = std::stod( InvocationMetadata["ContrastLeadTime"] );
                if(ContrastInjectionLeadTime < 0.0) throw std::runtime_error("Non-sensical 'ContrastLeadTime' found.");
                FUNCINFO("Found 'ContrastLeadTime' invocation metadata key. Using value " << ContrastInjectionLeadTime << "s");
            }
            auto PurgeAboveNSeconds = std::bind(PurgeAboveTemporalThreshold, std::placeholders::_1, ContrastInjectionLeadTime);

            for(auto & img_arr : orig_img_arrays){
                DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
                baseline_img_arrays.emplace_back( DICOM_data.image_data.back() );
    
                baseline_img_arrays.back()->imagecoll.Prune_Images_Satisfying(PurgeAboveNSeconds);
    
                if(!baseline_img_arrays.back()->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)){
                    FUNCERR("Cannot temporally average data set. Is it able to be averaged?");
                }
            }
    
        }else{
            //Baseline = minimum of signal over whole time course (minimum is usually pre-contrast, but noise
            // can affect the result).

            for(auto & img_arr : orig_img_arrays){
                DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
                baseline_img_arrays.emplace_back( DICOM_data.image_data.back() );
    
                if(!baseline_img_arrays.back()->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                                          CondenseMinPixel,
                                                                          { } )){
                    FUNCERR("Unable to generate min(pixel) images over the time course");
                }
            }
        }


        //Deep-copy the original long image array and use the temporally-averaged, pre-contrast map to work out the approximate contrast in each voxel.
        std::vector<std::shared_ptr<Image_Array>> C_enhancement_img_arrays;
        {
            auto img_arr = orig_img_arrays.front();
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            C_enhancement_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!C_enhancement_img_arrays.back()->imagecoll.Transform_Images( CTPerfusionSigDiffC,
                                                                             { baseline_img_arrays.front()->imagecoll },
                                                                             { } )){
                FUNCERR("Unable to transform image array to make poor-man's C map");
            }
        }


        //Temporally average the whole series, to convert motion to blur.
        std::vector<std::shared_ptr<Image_Array>> temporal_avg_img_arrays;
        if(false) for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            temporal_avg_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!temporal_avg_img_arrays.back()->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)){
                FUNCERR("Cannot temporally average large-pixel-censored data set. Is it able to be averaged?");
            }
        }

        // TODO: Temporally average in chunks of 3-5 overlappping images. This will give a cleaner time course
        //       but will not be totally independent of time.


        //Temporally average the C(t) map, to help assess whether it seems to conform to structures.
        std::vector<std::shared_ptr<Image_Array>> temp_avg_C_img_arrays;
        if(false) for(auto & img_arr : C_enhancement_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            temp_avg_C_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!temp_avg_C_img_arrays.back()->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)){
                FUNCERR("Cannot temporally average C map data set. Is it able to be averaged?");
            }
        }


        //Perform cluster analysis on the time contrast agent time courses.
        std::vector<std::shared_ptr<Image_Array>> clustered_img_arrays;
        if(false) for(auto & img_arr : C_enhancement_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            clustered_img_arrays.emplace_back( DICOM_data.image_data.back() );

            DBSCANTimeCoursesUserData UD;
            UD.MinPts = 10.0; //Used?
            UD.Eps = -1.0; //Used?
            if(!clustered_img_arrays.back()->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                                       DBSCANTimeCourses,
                                                                       cc_all,
                                                                       UD )){
                FUNCERR("Unable to perform DBSCAN clustering");
            }
        }


        //Deep-copy images at a single temporal point and highlight the ROIs. 
        if(false) if(!cc_all.empty()){
            std::vector<std::shared_ptr<Image_Array>> roi_highlighted_img_arrays;
            for(auto & img_arr : temporal_avg_img_arrays){
                DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
                roi_highlighted_img_arrays.emplace_back( DICOM_data.image_data.back() );

                if(!roi_highlighted_img_arrays.back()->imagecoll.Process_Images( GroupIndividualImages,
                                                                                 HighlightROIVoxels,
                                                                                 cc_all )){
                    FUNCERR("Unable to highlight ROIs");
                }
            }
        }


        //Copy the contrast agent images and generate contrast time courses for each ROI.
        if(false) if(!cc_all.empty()){

            std::vector<std::shared_ptr<Image_Array>> temp_img_arrays;
            //for(auto & img_arr : orig_img_arrays){
            for(auto & img_arr : C_enhancement_img_arrays){
                temp_img_arrays.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            }

            PerROITimeCoursesUserData ud;
            for(auto & img_arr : temp_img_arrays){
                if(!img_arr->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                       PerROITimeCourses,
                                                       cc_all,
                                                       &ud )){
                    FUNCERR("Unable to generate per-ROI time courses");
                }
            }
 
            //Plot the time courses.
            Plotter2 toplot;
            for(const auto &tc_pair : ud.time_courses){
                toplot.Set_Global_Title("Contrast agent time courses");
                toplot.Insert_samples_1D(tc_pair.second, tc_pair.first, "points");
                toplot.Insert_samples_1D(tc_pair.second, "", "linespoints");
            }
            toplot.Plot();
            toplot.Plot_as_PDF(Get_Unique_Sequential_Filename("/tmp/time_course_",4,".pdf"));
            WriteStringToFile(toplot.Dump_as_String(),
                              Get_Unique_Sequential_Filename("/tmp/time_course_gnuplot_",4,".dat"));
        }


        //Deep-copy and compute the max pixel intensity over the time course. This will help find the liver marker clips.
        std::vector<std::shared_ptr<Image_Array>> max_pixel_img_arrays;
        if(true) for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            max_pixel_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!max_pixel_img_arrays.back()->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                                       CondenseMaxPixel,
                                                                       { } )){
                FUNCERR("Unable to generate max(pixel) images over the time course");
            }
        }

        //Scale the pixel intensities on a logarithmic scale. (For viewing purposes only!)
        std::vector<std::shared_ptr<Image_Array>> log_scaled_img_arrays;
        if(true) for(auto & img_arr : max_pixel_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            log_scaled_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!log_scaled_img_arrays.back()->imagecoll.Process_Images( GroupIndividualImages,
                                                                        LogScalePixels,
                                                                        { } )){
                FUNCERR("Unable to perform logarithmic pixel scaling");
            }
        }

        ////Temporally average the images with logarithmically scaled pixels.
        //std::vector<std::shared_ptr<Image_Array>> temporalavgd_logscaled_img_arrays;
        //if(false) for(auto & img_arr : log_scaled_img_arrays){
        //    DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
        //    temporalavgd_logscaled_img_arrays.emplace_back( DICOM_data.image_data.back() );
        //
        //    if(!temporalavgd_logscaled_img_arrays.back()->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)){
        //        FUNCERR("Cannot temporally average data set. Is it able to be averaged?");
        //    }
        //}


        // IDEA: 1. Compute the MIN pixel value over the time course.
        //       2. Grow the bright areas of the MIN by N pixels in all directions. If the neighbouring
        //          pixel is lower, grow the pixel value out (but use the original MIN image as the growth
        //          map so the brightest pixel doesn't spread everywhere.)
        //       3. Take the full, original image series and subtract off the GROWN MIN.
        // This ought to help get rid of ribs, couch, anything consistently bright in every image.
        // Since the liver clips and liver move around quite a bit, they should be 'hidden' in the MIN.
        // Subtracting off the bright areas should really help ensure static structures do not remain.


        //Deep-copy and compute the min pixel intensity over the time course. This will help find the liver marker clips.
        std::vector<std::shared_ptr<Image_Array>> min_pixel_img_arrays;
        if(false) for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            min_pixel_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!min_pixel_img_arrays.back()->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                                       CondenseMinPixel,
                                                                       { } )){
                FUNCERR("Unable to generate min(pixel) images over the time course");
            }
        }


        //Deep-copy and subtract the min pixel intensity over the time course from each image. This will help find the liver marker clips.
        std::vector<std::shared_ptr<Image_Array>> sub_min_pixel_img_arrays;
        if(false) for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            sub_min_pixel_img_arrays.emplace_back( DICOM_data.image_data.back() );

            std::list<std::reference_wrapper<planar_image_collection<float,double>>> external_imgs;
            for(auto & img_arr2 : min_pixel_img_arrays){
                external_imgs.push_back( std::ref(img_arr2->imagecoll) );
            }
            if(!sub_min_pixel_img_arrays.back()->imagecoll.Transform_Images( SubtractSpatiallyOverlappingImages, std::move(external_imgs), {} )){
                FUNCERR("Unable to subtract the min(pixel) map from the time course");
            }
        }


        //TODO: Grow bright areas to N nearby pixels.


        //TODO: Alter this routine to use the GROWN MIN instead of the MIN directly.


        //Generate a map which will help in the identification of liver marker clips.
        std::vector<std::shared_ptr<Image_Array>> clip_likelihood_map_img_arrays;
        if(false) for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            clip_likelihood_map_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!clip_likelihood_map_img_arrays.back()->imagecoll.Process_Images( GroupIndividualImages,
                                                                                 CTPerfusionSearchForLiverClips,
                                                                                 { } )){
                FUNCERR("Unable to perform search for liver clip markers");
            }
        }

        //Deep-copy and temporally-average the clip likelihood maps. This is hopefully average out clip motions
        // over the time course (clip motion should be somewhat).   
        std::vector<std::shared_ptr<Image_Array>> tavgd_clip_likelihood_map_img_arrays;
        if(false) for(auto & img_arr : clip_likelihood_map_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            tavgd_clip_likelihood_map_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!tavgd_clip_likelihood_map_img_arrays.back()->imagecoll.Condense_Average_Images(
                                                                     GroupSpatiallyOverlappingImages)){
                FUNCERR("Unable to time-average clip likelihood maps");
            }
        }
 

    }


    //=================================================================================================================
    //============================================= UBC3TMRI TD03 DCE =================================================
    //=================================================================================================================
    if(Operations.count("UBC3TMRI_DCE_Experimental") != 0){

        //Get named handles for each image array so we can easily refer to them later.
        std::shared_ptr<Image_Array> dummy;
        std::shared_ptr<Image_Array> img_arr_orig_long_scan = *std::next(DICOM_data.image_data.begin(),0);  //SeriesNumber 901.
        std::vector<std::shared_ptr<Image_Array>> short_scans;
        for(auto it = std::next(DICOM_data.image_data.begin(),1);
                 it != DICOM_data.image_data.end(); ++it) short_scans.push_back( *it );
//        std::shared_ptr<Image_Array> img_arr_orig_short_scan = *std::next(DICOM_data.image_data.begin(),1); //SeriesNumber 801.


        //Temporally average the long array for later S0 and T1 map creation.
        DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_orig_long_scan ) );
        std::shared_ptr<Image_Array> img_arr_copy_long_temporally_avgd( DICOM_data.image_data.back() );

        double ContrastInjectionLeadTime = 35.0; //Seconds. 
        if(InvocationMetadata.count("ContrastLeadTime") == 0){
            FUNCWARN("Unable to locate 'ContrastLeadTime' invocation metadata key. Assuming the default lead time "
                     << ContrastInjectionLeadTime << "s is appropriate");
        }else{
            ContrastInjectionLeadTime = std::stod( InvocationMetadata["ContrastLeadTime"] );
            if(ContrastInjectionLeadTime < 0.0) throw std::runtime_error("Non-sensical 'ContrastLeadTime' found.");
            FUNCINFO("Found 'ContrastLeadTime' invocation metadata key. Using value " << ContrastInjectionLeadTime << "s");
        }
        auto PurgeAboveNSeconds = std::bind(PurgeAboveTemporalThreshold, std::placeholders::_1, ContrastInjectionLeadTime);

        img_arr_copy_long_temporally_avgd->imagecoll.Prune_Images_Satisfying(PurgeAboveNSeconds);
        if(!img_arr_copy_long_temporally_avgd->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)) FUNCERR("Cannot temporally avg long img_arr");

 
        //Temporally average the short arrays for later S0 and T1 map creation.
        std::vector<std::shared_ptr<Image_Array>> short_tavgd;
        for(auto img_ptr : short_scans){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_ptr ) );
            short_tavgd.push_back( DICOM_data.image_data.back() );
//            std::shared_ptr<Image_Array> img_arr_copy_short_temporally_avgd( DICOM_data.image_data.back() );

//        img_arr_copy_short_temporally_avgd->imagecoll.Prune_Images_Satisfying(PurgeAboveNSeconds);
            if(!short_tavgd.back()->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)) FUNCERR("Cannot temporally avg short img_arr");
        }
//        if(!img_arr_copy_short_temporally_avgd->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)) FUNCERR("Cannot temporally avg short img_arr");


        //Gaussian blur in pixel space.
        std::shared_ptr<Image_Array> img_arr_long_tavgd_blurred(img_arr_copy_long_temporally_avgd);
        if(false){ //Blur the image.
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_long_tavgd_blurred ) );
            img_arr_long_tavgd_blurred = DICOM_data.image_data.back();

            if(!img_arr_long_tavgd_blurred->imagecoll.Gaussian_Pixel_Blur({ }, 1.5)){
                FUNCERR("Unable to blur long temporally averaged images");
            }
        }

        std::vector<std::shared_ptr<Image_Array>> short_tavgd_blurred;
        if(false){
            for(auto img_ptr : short_tavgd){
                DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_ptr ) );
                short_tavgd_blurred.push_back( DICOM_data.image_data.back() );

                if(!short_tavgd_blurred.back()->imagecoll.Gaussian_Pixel_Blur({ }, 1.5)){
                    FUNCERR("Unable to blur short temporally averaged images");
                }
            }
        }else{
            for(auto img_ptr : short_tavgd) short_tavgd_blurred.push_back( img_ptr );
        }
//        std::shared_ptr<Image_Array> img_arr_short_tavgd_blurred(img_arr_copy_short_temporally_avgd);
//       if(true){ //Blur the image.
//            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_short_tavgd_blurred ) );
//            img_arr_short_tavgd_blurred = DICOM_data.image_data.back();
//        
//            if(!img_arr_short_tavgd_blurred->imagecoll.Gaussian_Pixel_Blur({ }, 1.5)){
//                FUNCERR("Unable to blur short temporally averaged images");
//            }
//        }
//

        //Package the short and long images together as needed for the S0 and T1 calculations.
        std::list<std::reference_wrapper<planar_image_collection<float,double>>> tavgd_blurred;
        tavgd_blurred.push_back(img_arr_long_tavgd_blurred->imagecoll);
        for(auto img_ptr : short_tavgd_blurred) tavgd_blurred.push_back(img_ptr->imagecoll);

        //Deep-copy and process the (possibly blurred) collated image array, generating a T1 map in-situ.
        DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_long_tavgd_blurred ) );
        std::shared_ptr<Image_Array> img_arr_T1_map( DICOM_data.image_data.back() );

        if(!img_arr_T1_map->imagecoll.Transform_Images( DCEMRIT1MapV2, tavgd_blurred, { } )){
            FUNCERR("Unable to transform image array to make T1 map");
        }

        //Produce an S0 map.
        DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_long_tavgd_blurred ) );
        std::shared_ptr<Image_Array> img_arr_S0_map( DICOM_data.image_data.back() );

        if(!img_arr_S0_map->imagecoll.Transform_Images( DCEMRIS0MapV2, tavgd_blurred, { } )){
            FUNCERR("Unable to transform image array to make S0 map");
        }

        //Blur the S0 and T1 maps if needed.
        std::shared_ptr<Image_Array> img_arr_T1_map_blurred( img_arr_T1_map );
        if(false){ //Produce a blurred image.
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_T1_map ) );
            img_arr_T1_map_blurred = DICOM_data.image_data.back();
        
            if(!img_arr_T1_map_blurred->imagecoll.Gaussian_Pixel_Blur({ }, 1.5)){
                FUNCERR("Unable to blur T1 map");
            }
        }

        std::shared_ptr<Image_Array> img_arr_S0_map_blurred( img_arr_S0_map );
        if(false){ //Produce a blurred image.
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_S0_map ) );
            img_arr_S0_map_blurred = DICOM_data.image_data.back();
            
            if(!img_arr_S0_map_blurred->imagecoll.Gaussian_Pixel_Blur({ }, 1.5)){
                FUNCERR("Unable to blur S0 map");
            }
        }


        //Compute the contrast agent enhancement C(t) curves using S0 and T1 maps.
        std::shared_ptr<Image_Array> img_arr_C_map; //Can be either "proper" or "signal difference" C(t) maps.
        if(true){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_orig_long_scan ) );
            img_arr_C_map = DICOM_data.image_data.back();

            if(!img_arr_C_map->imagecoll.Transform_Images( DCEMRICMap,
                                                           { img_arr_S0_map_blurred->imagecoll, img_arr_T1_map_blurred->imagecoll }, 
                                                           { } )){
                FUNCERR("Unable to transform image array to make C map");
            }

        //Or compute it using the signal difference method (without S0 or T1 maps).
        }else{
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_orig_long_scan ) );
            img_arr_C_map = DICOM_data.image_data.back();

            if(!img_arr_C_map->imagecoll.Transform_Images( DCEMRISigDiffC,
                                                           { img_arr_copy_long_temporally_avgd->imagecoll }, 
                                                           { } )){
                FUNCERR("Unable to transform image array to make poor-man's C map");
            }
        }

        //Compute an IAUC map from the C(t) map.
        if(false){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_C_map ) ); 
            std::shared_ptr<Image_Array> img_arr_iauc_map( DICOM_data.image_data.back() );
    
            if(!img_arr_iauc_map->imagecoll.Process_Images( GroupSpatiallyOverlappingImages, DCEMRIAUCMap, {} )){
                FUNCERR("Unable to process image array to make IAUC map");
            }
        }


        //Perform a "kitchen sink" analysis on the C(t) map.
        if(false){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_C_map ) ); 
            std::shared_ptr<Image_Array> img_arr_kitchen_sink_map( DICOM_data.image_data.back() );

            if(!img_arr_kitchen_sink_map->imagecoll.Process_Images( GroupSpatiallyOverlappingImages, 
                                                                    KitchenSinkAnalysis, 
                                                                    { cc_all } )){
                                                                    //{ cc_r_parotid_int, cc_l_parotid_int } )){
                                                                    //{ cc_r_parotid_int, cc_l_parotid_int, cc_r_masseter_int, cc_pharynx_int } )){
                FUNCERR("Unable to process image array to perform kitchen sink analysis");
            }else{
                DumpKitchenSinkResults(InvocationMetadata);
            }
        }
 
        //Compute a histogram over pixel value intensities for each ROI using the original long time series.
        if(false){
            if(!img_arr_orig_long_scan->imagecoll.Transform_Images( PixelHistogramAnalysis,
                                                                    { },
                                                                    { cc_all } )){
                                                                    //{ cc_r_parotid_int, cc_l_parotid_int } )){
                                                                    //{ cc_r_parotid_int, cc_l_parotid_int, cc_r_masseter_int, cc_pharynx_int } )){
                FUNCERR("Unable to compute pixel value intensity histograms");
            }else{
                DumpPixelHistogramResults();
            }
        }

        //Deep-copy images at a single temporal point and highlight the ROIs. 
        if(false){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_copy_long_temporally_avgd ) );
            std::shared_ptr<Image_Array> img_arr_highlighted_rois( DICOM_data.image_data.back() );

            if(!img_arr_highlighted_rois->imagecoll.Process_Images( GroupIndividualImages, 
                                                                    HighlightROIVoxels,
                                                                    { cc_all } )){
                                                                    //{ cc_r_parotid_int, cc_l_parotid_int, cc_r_masseter_int, cc_pharynx_int } )){
                FUNCERR("Unable to highlight ROIs");
            }
        }

    }

    //=================================================================================================================
    //============================================= UBC3TMRI Vol01 DCE ================================================
    //=================================================================================================================
    if(Operations.count("UBC3TMRI_DCE") != 0){
   
        //Get handles for each of the original image arrays so we can easily refer to them later.
        std::vector<std::shared_ptr<Image_Array>> orig_img_arrays;
        for(auto & img_arr : DICOM_data.image_data) orig_img_arrays.push_back(img_arr);

        //Get named handles for each image array so we can easily refer to them later.
        //std::shared_ptr<Image_Array> img_arr_orig_long_scan = *std::next(DICOM_data.image_data.begin(),0);
        //std::shared_ptr<Image_Array> img_arr_orig_short_scan = *std::next(DICOM_data.image_data.begin(),1);

        //Figure out how much time elapsed before contrast injection began.
        double ContrastInjectionLeadTime = 35.0; //Seconds. 
        if(InvocationMetadata.count("ContrastLeadTime") == 0){
            FUNCWARN("Unable to locate 'ContrastLeadTime' invocation metadata key. Assuming the default lead time " 
                     << ContrastInjectionLeadTime << "s is appropriate");
        }else{
            ContrastInjectionLeadTime = std::stod( InvocationMetadata["ContrastLeadTime"] );
            if(ContrastInjectionLeadTime < 0.0) throw std::runtime_error("Non-sensical 'ContrastLeadTime' found.");
            FUNCINFO("Found 'ContrastLeadTime' invocation metadata key. Using value " << ContrastInjectionLeadTime << "s"); 
        }

        //Deep-copy, trim the post-contrast injection signal, and temporally-average the image arrays.
        auto PurgeAboveNSeconds = std::bind(PurgeAboveTemporalThreshold, std::placeholders::_1, ContrastInjectionLeadTime);
        std::vector<std::shared_ptr<Image_Array>> temporal_avg_img_arrays;
        for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            temporal_avg_img_arrays.emplace_back( DICOM_data.image_data.back() );

            temporal_avg_img_arrays.back()->imagecoll.Prune_Images_Satisfying(PurgeAboveNSeconds);

            if(!temporal_avg_img_arrays.back()->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)){
                FUNCERR("Cannot temporally average data set. Is it able to be averaged?");
            }
        }

        //Deep-copy images at a single temporal point and highlight the ROIs. 
        if(!cc_all.empty()){
            std::vector<std::shared_ptr<Image_Array>> roi_highlighted_img_arrays;
            for(auto & img_arr : temporal_avg_img_arrays){
                DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
                roi_highlighted_img_arrays.emplace_back( DICOM_data.image_data.back() );

                if(!roi_highlighted_img_arrays.back()->imagecoll.Process_Images( GroupIndividualImages,
                                                                                 HighlightROIVoxels,
                                                                                 cc_all )){
                        //{ Custom_CCs["1.3.46.670589.11.5720.5.0.6024.2015042415010571009"]["1.3.46.670589.11.5720.5.0.6792.2015042414281765155"]["Hi_Test"] } )){
                    FUNCERR("Unable to highlight ROIs");
                }
            }
        }

        //Deep-copy temporally-averaged images and blur them.
        std::vector<std::shared_ptr<Image_Array>> tavgd_blurred;
        if(true){
            for(auto img_ptr : temporal_avg_img_arrays){
                DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_ptr ) );
                tavgd_blurred.push_back( DICOM_data.image_data.back() );

                if(!tavgd_blurred.back()->imagecoll.Gaussian_Pixel_Blur({ }, 1.5)){
                    FUNCERR("Unable to blur temporally averaged images");
                }
            }
        }else{
            for(auto img_ptr : temporal_avg_img_arrays) tavgd_blurred.push_back( img_ptr );
        }



        //Deep-copy the original long image array and use the temporally-averaged, pre-contrast map to work out the poor-man's Gad C in each voxel.
        std::vector<std::shared_ptr<Image_Array>> poormans_C_map_img_arrays;
        //for(auto & img_arr : orig_img_arrays){
        {
            auto img_arr = orig_img_arrays.front();
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            poormans_C_map_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!poormans_C_map_img_arrays.back()->imagecoll.Transform_Images( DCEMRISigDiffC,
                                                                              { tavgd_blurred.front()->imagecoll },
                                                                              { } )){
                FUNCERR("Unable to transform image array to make poor-man's C map");
            }
        }
        //poormans_C_map_img_arrays.resize(1); // <---- Assumes only the first image is 

        //DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr_orig_long_scan ) );
        //std::shared_ptr<Image_Array> img_arr_poormans_C_map( DICOM_data.image_data.back() );
        //
        //if(!img_arr_poormans_C_map->imagecoll.Transform_Images( DCEMRISigDiffC,
        //                                                        { img_arr_copy_long_temporally_avgd->imagecoll }, 
        //                                                        { } )){
        //    FUNCERR("Unable to transform image array to make poor-man's C map");
        //}



        //Deep-copy the poor-man's C(t) map and use the images to compute an IAUC map.
        //
        // NOTE: Takes a LONG time. You need to modify the IAUC code's Ygor... integration routine.
        //       I think it might be using a generic integration routine which samples the integrand 100 times
        //       between each datum(!). Surely you can improve this for a run-of-the-mill linear integrand.
        if(false){
            std::vector<std::shared_ptr<Image_Array>> iauc_c_map_img_arrays;
            for(auto & img_arr : poormans_C_map_img_arrays){
                DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
                iauc_c_map_img_arrays.emplace_back( DICOM_data.image_data.back() );

                if(!iauc_c_map_img_arrays.back()->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                                            DCEMRIAUCMap,
                                                                            {} )){
                    FUNCERR("Unable to process image array to make IAUC map");
                }
            }
        }

        //Deep-copy the poor-man's C(t) map and use the images to perform a "kitchen sink" analysis.
        if(false){
            std::vector<std::shared_ptr<Image_Array>> kitchen_sink_map_img_arrays;
            if(poormans_C_map_img_arrays.size() == 1){
                for(auto & img_arr : poormans_C_map_img_arrays){
                    DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
                    kitchen_sink_map_img_arrays.emplace_back( DICOM_data.image_data.back() );
                    
                    if(!kitchen_sink_map_img_arrays.back()->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                                                      KitchenSinkAnalysis,
                                                                                      cc_all )){
                        FUNCERR("Unable to process image array to perform kitchen sink analysis");
                    }else{
                        DumpKitchenSinkResults(InvocationMetadata);
                    }
                }
            }else{
                FUNCWARN("Skipping kitchen sink analysis. This routine uses static storage and assumes it will "
                         "be run over a single image array.");
            }
        }
    }

    //=================================================================================================================
    //========================================= UBC3TMRI DCE Difference Maps ==========================================
    //=================================================================================================================
    //This routine generates difference maps using both long DCE scans. Thus it takes up a LOT of memory! Try avoid 
    // unnecessary copies of large (temporally long) arrays.
    if(Operations.count("UBC3TMRI_DCE_Differences") != 0){

        if(DICOM_data.image_data.size() != 2) FUNCERR("Expected two image arrays in a specific order. Cannot continue");  
 
        //Get named handles for each image array so we can easily refer to them later.
        std::shared_ptr<Image_Array> orig_unstim_long  = *std::next(DICOM_data.image_data.begin(),0); //full (long) DCE 01 scan (no stim).
        std::shared_ptr<Image_Array> orig_stim_long    = *std::next(DICOM_data.image_data.begin(),1); //full (long) DCE 02 scan (stim).
        DICOM_data.image_data.clear(); //Trying to conserve space.


        //Deep-copy, trim the post-contrast injection signal, and temporally-average the image arrays.
        auto PurgeAbove35Seconds = std::bind(PurgeAboveTemporalThreshold, std::placeholders::_1, 35.0);

        std::shared_ptr<Image_Array> tavgd_unstim_long = std::make_shared<Image_Array>( *orig_unstim_long );
        tavgd_unstim_long->imagecoll.Prune_Images_Satisfying(PurgeAbove35Seconds);
        if(!tavgd_unstim_long->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)){
            FUNCERR("Cannot temporally average data set. Is it able to be averaged?");
        }

        std::shared_ptr<Image_Array> tavgd_stim_long = std::make_shared<Image_Array>( *orig_stim_long );
        tavgd_stim_long->imagecoll.Prune_Images_Satisfying(PurgeAbove35Seconds);
        if(!tavgd_stim_long->imagecoll.Condense_Average_Images(GroupSpatiallyOverlappingImages)){
            FUNCERR("Cannot temporally average data set. Is it able to be averaged?");
        }       


        //Deep-copy the original long image array and use the temporally-averaged, pre-contrast map to work out the poor-man's Gad C in each voxel.
        std::shared_ptr<Image_Array> unstim_C = std::make_shared<Image_Array>( *orig_unstim_long );
        if(!unstim_C->imagecoll.Transform_Images( DCEMRISigDiffC, { tavgd_unstim_long->imagecoll }, {}) ){
            FUNCERR("Unable to transform image array to make poor-man's C map");
        }
        orig_unstim_long.reset();
        //DICOM_data.image_data.emplace_back(unstim_C);
        
        std::shared_ptr<Image_Array> stim_C = std::make_shared<Image_Array>( *orig_stim_long );
        if(!stim_C->imagecoll.Transform_Images( DCEMRISigDiffC, { tavgd_stim_long->imagecoll }, {}) ){
            FUNCERR("Unable to transform image array to make poor-man's C map");
        }
        orig_stim_long.reset();
        //DICOM_data.image_data.emplace_back(stim_C);


        //Generate maps of the slope for the various time segments.
        auto TimeCourseSlopeDifferenceOverStim = std::bind(TimeCourseSlopeDifference, 
                                                 std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
                                                 135.0, 300.0,
                                                 300.0, std::numeric_limits<double>::max(),
                                                 std::experimental::any() );


        std::shared_ptr<Image_Array> nostim_case = std::make_shared<Image_Array>(*unstim_C);
        if(!nostim_case->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                   TimeCourseSlopeDifferenceOverStim,
                                                   cc_all )){
            FUNCERR("Unable to compute time course slope map");
        }
        unstim_C.reset(); 

        std::shared_ptr<Image_Array> stim_case = std::make_shared<Image_Array>(*stim_C);
        if(!stim_case->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
                                                 TimeCourseSlopeDifferenceOverStim,
                                                 cc_all )){
            FUNCERR("Unable to compute time course slope map");
        }
        stim_C.reset();

        DICOM_data.image_data.emplace_back(nostim_case);
        DICOM_data.image_data.emplace_back(stim_case);

        //Compute the difference of the images.
        //
        //std::shared_ptr<Image_Array> differenceA = std::make_shared<Image_Array>(*stim_case);
        //{
        //  std::list<std::reference_wrapper<planar_image_collection<float,double>>> external_imgs;
        //  external_imgs.push_back( std::ref(nostim_case->imagecoll) );
        //
        //  if(!differenceA->imagecoll.Transform_Images( SubtractSpatiallyOverlappingImages, std::move(external_imgs), {} )){
        //      FUNCERR("Unable to subtract the pixel maps");
        //  }
        //}
        //DICOM_data.image_data.emplace_back(differenceA);
        //
        //std::shared_ptr<Image_Array> differenceB = std::make_shared<Image_Array>(*nostim_case);
        //{
        //  std::list<std::reference_wrapper<planar_image_collection<float,double>>> external_imgs;
        //  external_imgs.push_back( std::ref(stim_case->imagecoll) );
        //
        //  if(!differenceB->imagecoll.Transform_Images( SubtractSpatiallyOverlappingImages, std::move(external_imgs), {} )){
        //      FUNCERR("Unable to subtract the pixel maps");
        //  }
        //}
        //DICOM_data.image_data.emplace_back(differenceB);


        std::shared_ptr<Image_Array> difference = std::make_shared<Image_Array>(*stim_case);
        {
          std::list<std::reference_wrapper<planar_image_collection<float,double>>> external_imgs;
          external_imgs.push_back( std::ref(nostim_case->imagecoll) );

          if(!difference->imagecoll.Transform_Images( SubtractSpatiallyOverlappingImages, std::move(external_imgs), {} )){
              FUNCERR("Unable to subtract the pixel maps");
          }
        }


        DICOM_data.image_data.emplace_back(difference);

        //Compute moments for the ROIs.
        //auto ComputeCentralizedMomentsFloatPacked = std::bind(ComputeCentralizedMoments,
        //                                                      std::placeholders::_1, std::placeholders::_2, std::placeholders::_3,
        //                                                      true); //Treat pixels as floats packed into uint32_t pixels.
        //
        //std::shared_ptr<Image_Array> moments = std::make_shared<Image_Array>(*difference);
        //if(!moments->imagecoll.Process_Images( GroupSpatiallyOverlappingImages,
        //                                        ComputeCentralizedMomentsFloatPacked,
        //                                        cc_all )){
        //    FUNCERR("Unable to process image array to perform centralized moment analysis");
        //}else{
        //    DumpCentralizedMoments(InvocationMetadata);
        //}
        //DICOM_data.image_data.emplace_back(moments);
    }

    //=================================================================================================================
    //============================================= Image Routine Tests ===============================================
    //=================================================================================================================
    if(Operations.count("ImageRoutineTests") != 0){

        //Get handles for each of the original image arrays so we can easily refer to them later.
        std::vector<std::shared_ptr<Image_Array>> orig_img_arrays;
        for(auto & img_arr : DICOM_data.image_data) orig_img_arrays.push_back(img_arr);


        //Deep-copy, resample the original images using bilinear interpolation, for image viewing, contours, etc..
        std::vector<std::shared_ptr<Image_Array>> bilin_resampled_img_arrays;
        for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            bilin_resampled_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!bilin_resampled_img_arrays.back()->imagecoll.Process_Images( GroupIndividualImages,
                                                                             InImagePlaneBilinearSupersample,
                                                                             { } )){
                FUNCERR("Unable to bilinearly supersample images");
            }
        }

        //Deep-copy, resample the original images using bicubic interpolation, for image viewing, contours, etc..
        std::vector<std::shared_ptr<Image_Array>> bicub_resampled_img_arrays;
        if(false) for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            bicub_resampled_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!bicub_resampled_img_arrays.back()->imagecoll.Process_Images( GroupIndividualImages,
                                                                             InImagePlaneBicubicSupersample,
                                                                             { } )){
                FUNCERR("Unable to bicubically supersample images");
            }
        }


        //Deep-copy, convert the original images to their 'cross' second-order partial derivative (for edge-finding).
        std::vector<std::shared_ptr<Image_Array>> cross_second_deriv_img_arrays;
        for(auto & img_arr : orig_img_arrays){
            DICOM_data.image_data.emplace_back( std::make_shared<Image_Array>( *img_arr ) );
            cross_second_deriv_img_arrays.emplace_back( DICOM_data.image_data.back() );

            if(!cross_second_deriv_img_arrays.back()->imagecoll.Process_Images( GroupIndividualImages,
                                                                                CrossSecondDerivative,
                                                                                { } )){
                FUNCERR("Unable to compute 'cross' second-order partial derivative");
            }
        }


    }

    //Launch an interactive viewing window.
    if(Operations.count("View") != 0){
        if(DICOM_data.image_data.empty()) FUNCERR("No image data available to view. Cannot continue");

        //Produce a little sound to notify the user we've started showing something.
        sf::Music music;
        {
            const std::vector<std::string> soundpaths = { "Sounds/Ready.ogg",
               "/home/hal/Dropbox/Project - DICOMautomaton/Sounds/Ready.ogg",
               "/tmp/Ready.ogg", "Ready.ogg" };
            bool worked = false;
            for(const auto &soundpath : soundpaths){
                if(!music.openFromFile(soundpath)) continue;
                worked = true;
                music.play();
                break;
            }
            if(!worked){
                FUNCWARN("Unable to play notification sound. Continuing anyways");
            }
        }


        //If, for some reason, several image arrays are available for viewing, we need to provide a means for stepping
        // through the arrays. 
        //
        // NOTE: The reasoning for having several image arrays is not clear cut. If the timestamps are known exactly, it
        //       might make sense to split in this way. In general, it is up to the user to make this call. 
        typedef decltype(DICOM_data.image_data.begin()) img_data_ptr_it_t;
        img_data_ptr_it_t img_array_ptr_beg  = DICOM_data.image_data.begin();
        img_data_ptr_it_t img_array_ptr_end  = DICOM_data.image_data.end();
        img_data_ptr_it_t img_array_ptr_last = std::prev(DICOM_data.image_data.end());
        img_data_ptr_it_t img_array_ptr_it   = img_array_ptr_beg;

        //At the moment, we keep a single 'display' image active at a time. To help walk through the neighbouring images
        // (and the rest of the images, for that matter) we keep a container iterator to the image.
        typedef decltype(DICOM_data.image_data.front()->imagecoll.images.begin()) disp_img_it_t;
        disp_img_it_t disp_img_beg  = (*img_array_ptr_it)->imagecoll.images.begin();
        //disp_img_it_t disp_img_end  = (*img_array_ptr_it)->imagecoll.images.end();  // Not used atm.
        disp_img_it_t disp_img_last = std::prev((*img_array_ptr_it)->imagecoll.images.end());
        disp_img_it_t disp_img_it   = disp_img_beg;

        //Because SFML requires us to keep a sf::Texture alive for the duration of a sf::Sprite, we simply package them
        // together into a texture-sprite bundle. This means a single sf::Sprite must be accompanied by a sf::Texture, 
        // though in general several sf::Sprites could be bound to a sf::Texture.
        typedef std::pair<sf::Texture,sf::Sprite> disp_img_texture_sprite_t;
        disp_img_texture_sprite_t disp_img_texture_sprite;

        //Flags for various things.
        bool DumpScreenshot = false; //One-shot instruction to dump a screenshot immediately after rendering.
        bool OnlyShowTagsDifferentToNeighbours = true;

        //Accumulation-type storage.
        //contours_with_meta pixel_contours;     //Stores contours in terms of the Row,Col,SliceLocation (pixel) coords.
        //contour_of_points<double> pixel_contours_shtl;  //Stores contours in the DICOM coordinate system.
        //pixel_contours_shtl.closed = true;

        contours_with_meta contour_coll_shtl; //Stores contours in the DICOM coordinate system.
        contour_coll_shtl.contours.emplace_back();    //Prime the shuttle with an empty contour.
        contour_coll_shtl.contours.back().closed = true;

        std::list<sf::Vector2f> disp_pixel_contours;

        //Open a window.
        sf::RenderWindow window;
        window.create(sf::VideoMode(2000, 2000), "DICOMautomaton Image Viewer");
        window.setFramerateLimit(60);

        if(auto ImageDesc = disp_img_it->GetMetadataValueAs<std::string>("Description")){
            window.setTitle("DICOMautomaton IV: '"_s + ImageDesc.value() + "'");
        }else{
            window.setTitle("DICOMautomaton IV: <no description available>");
        }

        //Attempt to load fonts. We should try a few different files, and include a back-up somewhere accessible...
        sf::Font afont;
        if(!afont.loadFromFile("/usr/share/fonts/TTF/cmr10.ttf")) FUNCERR("Unable to find font file");

        //Create some primitive shapes, textures, and text objects for display later.
        //sf::CircleShape ashape(100.0f);
        sf::CircleShape smallcirc(1.0f);
        //ashape.setFillColor(sf::Color::Green);
        smallcirc.setFillColor(sf::Color::Green);

        sf::Text cursortext;
        cursortext.setFont(afont);
        //cursortext.setString("This is some trial text.\nWill the newline be handled properly?");
        cursortext.setString("");
        cursortext.setCharacterSize(15); //Size in pixels, not in points.
        //cursortext.setColor(sf::Color::Red);
        cursortext.setColor(sf::Color::Green);
        //cursortext.setStyle(sf::Text::Bold | sf::Text::Underlined);

        sf::Text BRcornertext;
        std::stringstream BRcornertextss;
        BRcornertext.setFont(afont);
        BRcornertext.setString("");
        BRcornertext.setCharacterSize(9); //Size in pixels, not in points.
        BRcornertext.setColor(sf::Color::Red);

        sf::Text BLcornertext;
        std::stringstream BLcornertextss;
        BLcornertext.setFont(afont);
        BLcornertext.setString("");
        BLcornertext.setCharacterSize(15); //Size in pixels, not in points.
        BLcornertext.setColor(sf::Color::Blue);

        

        const auto load_img_texture_sprite = [&](const disp_img_it_t &img_it, disp_img_texture_sprite_t &out) -> bool {
            //This routine returns a pair of (texture,sprite) because the texture must be kept around
            // for the duration of the sprite.
            const auto img_cols = img_it->columns;
            const auto img_rows = img_it->rows;

            if(!isininc(1,img_rows,10000) || !isininc(1,img_cols,10000)){
                FUNCERR("Image dimensions are not reasonable. Is this a mistake? Refusing to continue");
            }

            sf::Image animage;
            animage.create(img_cols, img_rows);

            //------------------------------------------------------------------------------------------------
            //Apply a window to the data if it seems like the WindowCenter or WindowWidth specified in the image metadata
            // are applicable. Note that it is likely that pixels will be clipped or truncated. This is intentional.
            auto win_valid = img_it->GetMetadataValueAs<std::string>("WindowValidFor");
            auto desc      = img_it->GetMetadataValueAs<std::string>("Description");
            auto win_c     = img_it->GetMetadataValueAs<double>("WindowCenter");
            auto win_fw    = img_it->GetMetadataValueAs<double>("WindowWidth"); //Full width or range. (Diameter, not radius.)

            if( win_valid && desc && win_c && win_fw && (win_valid.value() == desc.value()) ){
                //Window/linear scaling transformation parameters.
                //const auto win_r = 0.5*(win_fw.value() - 1.0); //The 'radius' of the range, or half width omitting the centre point.
                const auto win_r = 0.5*win_fw.value(); //The 'radius' of the range, or half width omitting the centre point.
    
                //The output range we are targeting. In this case, a commodity 8 bit (2^8 = 256 intensities) display.
                const double destmin = static_cast<double>( 0 );
                const double destmax = static_cast<double>( std::numeric_limits<uint8_t>::max() );
    
                for(auto i = 0; i < img_cols; ++i){
                    for(auto j = 0; j < img_rows; ++j){
                        const auto val = static_cast<double>( img_it->value(j,i,0) ); //The first (R or gray) channel.
                        if(!std::isfinite(val)){
                            animage.setPixel(i,j,sf::Color(255,0,0));
                        }else{
                            double y = destmin; //The new value of the pixel.
    
                            //If above or below the cutoff range, the pixel could be treated as the window min/max or simply as if
                            // it did not exist. We could set the value to fully transparent, NaN, or a specially designated 
                            // 'ignore' colour. It's ultimately up to the user.
                            if(val <= (win_c.value() - win_r)){
                                y = destmin;
                            }else if(val >= (win_c.value() + win_r)){
                                //Logical choice, but make viewing hard if window is too low...
                                y = destmax;
    
                            //If within the window range. Scale linearly as the pixel's position in the window.
                            }else{
                                //const double clamped = (val - (win_c.value() - win_r)) / (2.0*win_r + 1.0);
                                const double clamped = (val - (win_c.value() - win_r)) / (win_fw.value());
                                y = clamped * (destmax - destmin) + destmin;
                            }
    
                            const auto scaled_value = static_cast<uint8_t>( std::floor(y) );
                            animage.setPixel(i,j,sf::Color(scaled_value,scaled_value,scaled_value));
                        }
                    }
                }

            //------------------------------------------------------------------------------------------------
            //Scale pixels to fill the maximum range. None will be clipped or truncated.
            }else{
                //Due to a strange dependence on windowing, some manufacturers spit out massive pixel values.
                // If you don't want to window you need to anticipate and ignore the gigantic numbers being 
                // you might encounter. This is not the place to do this! If you need to do it here, write a
                // filter routine and *call* it from here.
                //
                // NOTE: This routine could definitely use a re-working, especially to make it safe for all
                //       arithmetical types (i.e., handling negatives, ensuring there is no overflow or wrap-
                //       around, ensuring there is minimal precision loss).
                typedef decltype(img_it->value(0,0,0)) pixel_value_t;
                auto lowest  = std::numeric_limits<pixel_value_t>::max();
                auto highest = std::numeric_limits<pixel_value_t>::min();
                for(auto i = 0; i < img_cols; ++i){
                    for(auto j = 0; j < img_rows; ++j){
                        const auto value = img_it->value(j,i,0); //Gray channel, or R channel.
//                        const auto value = img_it->value(i,j,0); //Gray channel, or R channel.
                        lowest  = std::min(lowest,value);
                        highest = std::max(highest,value);
                    }
                }
                const auto pixel_type_max = static_cast<double>(std::numeric_limits<pixel_value_t>::max());
                const auto pixel_type_min = static_cast<double>(std::numeric_limits<pixel_value_t>::min());
                const auto dest_type_max = static_cast<double>(std::numeric_limits<uint8_t>::max()); //Min is implicitly 0.

                const double clamped_low  = static_cast<double>(lowest )/pixel_type_max;
                const double clamped_high = static_cast<double>(highest)/pixel_type_max;
    
                for(auto i = 0; i < img_cols; ++i){ 
                    for(auto j = 0; j < img_rows; ++j){ 
                        const auto val = img_it->value(j,i,0);
                        if(!std::isfinite(val)){
                            animage.setPixel(i,j,sf::Color(255,0,0));
                        }else{
                            const double clamped_value = (static_cast<double>(val) - pixel_type_min)/(pixel_type_max - pixel_type_min);
                            const auto rescaled_value = (clamped_value - clamped_low)/(clamped_high - clamped_low);
                            const auto scaled_value = static_cast<uint8_t>(rescaled_value * dest_type_max);
                            animage.setPixel(i,j,sf::Color(scaled_value,scaled_value,scaled_value));
                        }
                    }
                }
            }
            

            out.first = sf::Texture();
            out.second = sf::Sprite();
            if(!out.first.create(img_cols, img_rows)) FUNCERR("Unable to create empty SFML texture");
            if(!out.first.loadFromImage(animage)) FUNCERR("Unable to create SFML texture from planar_image");
            //out.first.setSmooth(true);        
            out.first.setSmooth(false);        
            out.second.setTexture(out.first);


            //Scale the displayed pixel aspect ratio if the image pxl_dx and pxl_dy differ.
            {
                const auto ImagePixelAspectRatio = img_it->pxl_dy / img_it->pxl_dx;
                out.second.setScale(1.0f,ImagePixelAspectRatio);
            }

            return true;
        };

        //Scale the image to fill the available space.
        const auto scale_sprite_to_fill_screen = [](const sf::RenderWindow &awindow, 
                                                    const disp_img_it_t &img_it, 
                                                    disp_img_texture_sprite_t &asprite) -> void {
            //Reset the scale if there is one. (Is this a compound scale, or absolute scale? It isn't clear.)
            //asprite.second.setScale(1.0f,1.0f);
 
            //Scale the displayed pixel aspect ratio if the image pxl_dx and pxl_dy differ.
            {
                const auto ImagePixelAspectRatio = img_it->pxl_dx / img_it->pxl_dy;
                asprite.second.setScale(1.0f,ImagePixelAspectRatio);
            }


            //Get the current bounding box size in 'global' coordinates.
            sf::FloatRect img_bb = asprite.second.getGlobalBounds();

            //Get the current window's view's (aka the camera's) viewport coordinates.
            sf::IntRect win_bb = awindow.getViewport( awindow.getView() );

            //Determine how much we can scale the image while keeping it visible.
            // We also need to keep the aspect ratio the same.
            float w_scale = 1.0;
            float h_scale = 1.0;
            h_scale = static_cast<float>(win_bb.height) / img_bb.height;
            w_scale = static_cast<float>(win_bb.width) / img_bb.width;
            h_scale = std::min(h_scale,w_scale);
            w_scale = std::min(h_scale,w_scale);

            //Actually scale the image.
            //asprite.second.setScale(w_scale,h_scale);
            asprite.second.scale(w_scale,h_scale);
            return;
        };

        //Prep the first image.
        if(!load_img_texture_sprite(disp_img_it, disp_img_texture_sprite)){
            FUNCERR("Unable to load image --> texture --> sprite");
        }
        scale_sprite_to_fill_screen(window,disp_img_it,disp_img_texture_sprite);


        //Run until the window is closed or the user wishes to exit.
        while(window.isOpen()){
            BRcornertextss.str(""); //Clear stringstream.
            //BLcornertextss.str(""); //Clear stringstream.

            //Check if any events have accumulated since the last poll. If so, deal with them.
            sf::Event event;
            while(window.pollEvent(event)){
                if(event.type == sf::Event::Closed){
                    window.close();
                }else if(window.hasFocus() && (event.type == sf::Event::KeyPressed)){
                    //FUNCINFO("Pressed key");
                    //std::cout << "control:" << event.key.control << std::endl;
                    //std::cout << "alt:" << event.key.alt << std::endl;
                    //std::cout << "shift:" << event.key.shift << std::endl;
                    //std::cout << "system:" << event.key.system << std::endl;

                    if(event.key.code == sf::Keyboard::Escape){
                        window.close();
                    }

                }else if(window.hasFocus() && (event.type == sf::Event::KeyReleased)){
                    //FUNCINFO("Released key");

                }else if(window.hasFocus() && (event.type == sf::Event::TextEntered) 
                                           && (event.text.unicode < 128)){
                    //Not the same as KeyPressed + KeyReleased. Think unicode characters, or control keys.
                    const auto thechar = static_cast<char>(event.text.unicode);
                    //FUNCINFO("ASCII character typed: '" << thechar << "'");

                    //Set the flag for dumping the window contents as an image after the next render.
                    if( thechar == 'd' ){
                        DumpScreenshot = true;


                    //Dump raw pixels for all spatially overlapping images from the current array.
                    // (Useful for dumping time courses.)
                    }else if( thechar == 'D' ){

                        //Get a list of images which spatially overlap this point. Order should be maintained.
                        //Simple way. 
                        //auto encompassing_images = (*img_array_ptr_it)->imagecoll.get_images_which_encompass_point(pix_pos);
                        //More reliable way.
                        const auto pix_pos = disp_img_it->position(0,0);
                        const auto ortho = disp_img_it->row_unit.Cross( disp_img_it->col_unit ).unit();
                        const std::list<vec3<double>> points = { pix_pos, pix_pos + ortho * disp_img_it->pxl_dz * 0.25,
                                                                          pix_pos - ortho * disp_img_it->pxl_dz * 0.25 };
                        auto encompassing_images = (*img_array_ptr_it)->imagecoll.get_images_which_encompass_all_points(points);

                        long int count = 0;
                        for(auto & pimg : encompassing_images){
                            const auto pixel_dump_filename_out = Get_Unique_Sequential_Filename("/tmp/raw_pixel_dump_uint16_scaled_per_chan_",6,".gray");
                            //if(Dump_Casted_Scaled_Pixels<float,double,uint16_t>(*pimg,pixel_dump_filename_out,YgorImageIOPixelScaling::TypeMinMax)){
                            //if(pimg->Dump_u16_scale_Pixels(pixel_dump_filename_out)){
                            if(Dump_Pixels(*pimg,pixel_dump_filename_out)){
                                FUNCINFO("Dumped pixel data for image " << count << " to file '" << pixel_dump_filename_out << "'");
                            }else{
                                FUNCWARN("Unable to dump pixel data for this image to file '" << pixel_dump_filename_out << "'");
                            }
                            ++count;
                        }
                        FUNCINFO("To convert them issue something like 'convert -size 256x256 -depth 16 "
                                 "-define quantum:format=unsigned -type grayscale image.gray -depth 16 ... out.jpg'");


                    //Dump raw pixels from the current image to file.
                    }else if(thechar == 'i'){
                        //const auto pixel_dump_filename_out = Get_Unique_Sequential_Filename("/tmp/raw_pixel_dump_uint16_scaled_per_chan_",6,".gray");
                        const auto pixel_dump_filename_out = Get_Unique_Sequential_Filename("/tmp/display_image_dump_",6,".fits");
                        //const auto pixel_dump_filename_out = Get_Unique_Sequential_Filename("/tmp/raw_pixel_dump_d64_per_chan_",6,".gray");
                        //if(Dump_Casted_Scaled_Pixels<float,double,uint16_t>(*disp_img_it,pixel_dump_filename_out,YgorImageIOPixelScaling::TypeMinMax)){
                        //if(Dump_Pixels(*disp_img_it,pixel_dump_filename_out)){
                        if(WriteToFITS(*disp_img_it,pixel_dump_filename_out)){
                            FUNCINFO("Dumped pixel data for this image to file '" << pixel_dump_filename_out << "'");
                        }else{
                            FUNCWARN("Unable to dump pixel data for this image to file '" << pixel_dump_filename_out << "'");
                        }

                    //Dump raw pixels from all images in the current array to file.
                    }else if(thechar == 'I'){
                        long int count = 0;
                        for(auto &pimg : (*img_array_ptr_it)->imagecoll.images){
                            const auto pixel_dump_filename_out = Get_Unique_Sequential_Filename("/tmp/image_dump_",6,".fits");
                            //if(Dump_Casted_Scaled_Pixels<float,double,uint16_t>(*pimg,pixel_dump_filename_out,YgorImageIOPixelScaling::TypeMinMax)){
                            //if(Dump_Pixels(pimg,pixel_dump_filename_out)){
                            if(WriteToFITS(pimg,pixel_dump_filename_out)){
                                FUNCINFO("Dumped pixel data for image " << count << " to file '" << pixel_dump_filename_out << "'");
                            }else{
                                FUNCWARN("Unable to dump pixel data for this image to file '" << pixel_dump_filename_out << "'");
                            }
                            ++count;
                        }
                        //FUNCINFO("To convert them issue something like 'convert -size 256x256 -depth 16 "
                        //         "-define quantum:format=unsigned -type grayscale image.gray -depth 16 ... out.jpg'");

                    //Given the current mouse coordinates, dump pixel intensity profiles along the current row and column.
                    //
                    }else if( (thechar == 'r') || (thechar == 'R') 
                          ||  (thechar == 'c') || (thechar == 'C') ){
                        //Get the *realtime* current mouse coordinates.
                        const sf::Vector2i curr_m_pos = sf::Mouse::getPosition(window);

                        //Convert to SFML/OpenGL "World" coordinates. 
                        sf::Vector2f curr_m_pos_w = window.mapPixelToCoords(curr_m_pos);

                        //Check if the mouse is within the image bounding box. We can only proceed if it is.
                        sf::FloatRect disp_img_bbox = disp_img_texture_sprite.second.getGlobalBounds();
                        if(!disp_img_bbox.contains(curr_m_pos_w)){
                            FUNCWARN("The mouse is not currently hovering over the image. Cannot dump row/column profiles");
                            break;
                        }

                        //Assuming the image is not rotated or skewed (though possibly scaled), determine which image pixel
                        // we are hovering over.
                        const auto clamped_col_as_f = fabs(curr_m_pos_w.x - disp_img_bbox.left)/disp_img_bbox.width;
                        const auto clamped_row_as_f = fabs(disp_img_bbox.top - curr_m_pos_w.y )/disp_img_bbox.height;

                        const auto img_w_h = disp_img_texture_sprite.first.getSize();
                        const auto col_as_u = static_cast<long int>(clamped_col_as_f * static_cast<float>(img_w_h.x));
                        const auto row_as_u = static_cast<long int>(clamped_row_as_f * static_cast<float>(img_w_h.y));
                        FUNCINFO("Dumping row and column profiles for row,col = " << row_as_u << "," << col_as_u);

                        //Cycle over the images, dumping the pixel value(s) and the 'dt' metadata value, if available.
                        //Single-pixel.
                        samples_1D<double> row_profile, col_profile;
                        //std::stringstream row_profile_title, col_profile_title;
                        std::stringstream title;

                        for(auto i = 0; i < disp_img_it->columns; ++i){
                            const auto val_raw = disp_img_it->value(row_as_u,i,0);
                            const auto col_num = static_cast<double>(i);
                            //const auto val_num = (val_raw < std::numeric_limits<uint16_t>::max()) ? static_cast<double>(val_raw) : -1.0;
                            col_profile.push_back({ col_num, 0.0, val_raw, 0.0 });
                        }
                        for(auto i = 0; i < disp_img_it->rows; ++i){
                            const auto val_raw = disp_img_it->value(i,col_as_u,0);
                            const auto row_num = static_cast<double>(i);
                            //const auto val_num = (val_raw < std::numeric_limits<uint16_t>::max()) ? static_cast<double>(val_raw) : -1.0;
                            row_profile.push_back({ row_num, 0.0, val_raw, 0.0 });
                        }
                        //row_profile_title << "Row profile for col = " << col_as_u << ". ";
                        //col_profile_title << "Col profile for row = " << row_as_u << ". ";

                        //row_profile.Plot(row_profile_title.str());
                        //col_profile.Plot(col_profile_title.str());

                        title << "Row and Column profile. (row,col) = (" << row_as_u << "," << col_as_u << ").";
                        try{
                            YgorMathPlotting::Shuttle<samples_1D<double>> row_shtl(row_profile, "Row Profile");
                            YgorMathPlotting::Shuttle<samples_1D<double>> col_shtl(col_profile, "Col Profile");
                            YgorMathPlotting::Plot<double>({row_shtl, col_shtl}, title.str(), "Pixel Index (row or col)", "Pixel Intensity");
                        }catch(const std::exception &e){
                            FUNCINFO("Failed to plot: " << e.what());
                        }

                        //Plotter2 rowplot;
                        //rowplot.Insert_samples_1D(row_profile, "", "linespoints");
                        //rowplot.Set_Global_Title(row_profile_title.str());
                        //rowplot.Plot();
                        //rowplot.Plot_as_PDF(Get_Unique_Sequential_Filename("/tmp/pixel_intensity_row_profile_for_col_"_s + Xtostring(col_as_u) + "_",6,".pdf"));

                        //Plotter2 colplot;
                        //colplot.Insert_samples_1D(col_profile, "", "linespoints");
                        //colplot.Set_Global_Title(col_profile_title.str());
                        //colplot.Plot();
                        //colplot.Plot_as_PDF(Get_Unique_Sequential_Filename("/tmp/pixel_intensity_col_profile_for_row_"_s + Xtostring(row_as_u) + "_",6,".pdf"));


                    //Given the current mouse coordinates, dump a time series at the image pixel over all available images
                    // which spatially overlap.
                    //
                    // So this routine dumps a time course at the mouse pixel.
                    //
                    }else if( (thechar == 't') || (thechar == 'T') ){
                        //Get the *realtime* current mouse coordinates.
                        const sf::Vector2i curr_m_pos = sf::Mouse::getPosition(window);

                        //Convert to SFML/OpenGL "World" coordinates. 
                        sf::Vector2f curr_m_pos_w = window.mapPixelToCoords(curr_m_pos);

                        //Check if the mouse is within the image bounding box. We can only proceed if it is.
                        sf::FloatRect disp_img_bbox = disp_img_texture_sprite.second.getGlobalBounds();
                        if(!disp_img_bbox.contains(curr_m_pos_w)){
                            FUNCWARN("The mouse is not currently hovering over the image. Cannot dump time course");
                            break;
                        }

                        //Assuming the image is not rotated or skewed (though possibly scaled), determine which image pixel
                        // we are hovering over.
                        const auto clamped_col_as_f = fabs(curr_m_pos_w.x - disp_img_bbox.left)/disp_img_bbox.width;
                        const auto clamped_row_as_f = fabs(disp_img_bbox.top - curr_m_pos_w.y )/disp_img_bbox.height;

                        const auto img_w_h = disp_img_texture_sprite.first.getSize();
                        const auto col_as_u = static_cast<uint32_t>(clamped_col_as_f * static_cast<float>(img_w_h.x));
                        const auto row_as_u = static_cast<uint32_t>(clamped_row_as_f * static_cast<float>(img_w_h.y));
                        FUNCINFO("Dumping time course for row,col = " << row_as_u << "," << col_as_u);

                        //Get the YgorImage (DICOM) pixel coordinates.
                        const auto pix_pos = disp_img_it->position(row_as_u,col_as_u);

                        //Get a list of images which spatially overlap this point. Order should be maintained.
                        //Simple way. 
                        //auto encompassing_images = (*img_array_ptr_it)->imagecoll.get_images_which_encompass_point(pix_pos);
                        //More reliable way.
                        const auto ortho = disp_img_it->row_unit.Cross( disp_img_it->col_unit ).unit();
                        const std::list<vec3<double>> points = { pix_pos, pix_pos + ortho * disp_img_it->pxl_dz * 0.25,
                                                                          pix_pos - ortho * disp_img_it->pxl_dz * 0.25 };
                        auto encompassing_images = (*img_array_ptr_it)->imagecoll.get_images_which_encompass_all_points(points);

                        //Cycle over the images, dumping the ordinate (pixel values) vs abscissa (time) derived from metadata.
                        samples_1D<double> shtl;
                        const std::string quantity("dt"); //As it appears in the metadata. Must convert to a double!
                        //const std::string quantity("Diffusion_bValue");

                        const double radius = 2.1; //Circle of certain radius (in DICOM-coord. system).
                        std::stringstream title;
                        title << "P_{row,col,rad} = P_{" << row_as_u << "," << col_as_u << "," << radius << "}";
                        title << " vs " << quantity << ". ";

                        for(const auto &enc_img_it : encompassing_images){
                            if(auto abscissa = enc_img_it->GetMetadataValueAs<double>(quantity)){
                                //Circle of certain radius (in DICOM-coord. system).
                                std::list<double> vals;
                                for(auto lrow = 0; lrow < enc_img_it->rows; ++lrow){
                                    for(auto lcol = 0; lcol < enc_img_it->columns; ++lcol){
                                        const auto row_col_pix_pos = enc_img_it->position(lrow,lcol);
                                        if(pix_pos.distance(row_col_pix_pos) <= radius){
                                            const auto pix_val = enc_img_it->value(lrow,lcol,0);
                                            if(std::isfinite(pix_val)) vals.push_back( static_cast<double>(pix_val) );
                                        }
                                    }
                                }
                                const auto dabscissa = 0.0;
                                const auto ordinate  = Stats::Mean(vals);
                                const auto dordinate = (vals.size() > 2) ? std::sqrt(Stats::Unbiased_Var_Est(vals))/std::sqrt(1.0*vals.size()) 
                                                                         : 0.0;
                                shtl.push_back(abscissa.value(),dabscissa,ordinate,dordinate);
                            }
                        }


                        title << "Time Course. Images encompass " << pix_pos << ". ";
                        try{
                            YgorMathPlotting::Shuttle<samples_1D<double>> ymp_shtl(shtl, "Buffer A");
                            YgorMathPlotting::Plot<double>({ymp_shtl}, title.str(), "Time (s)", "Pixel Intensity");
                        }catch(const std::exception &e){
                            FUNCINFO("Failed to plot: " << e.what());
                        }

                        //Plotter2 toplot;
                        //title << "Images encompass " << pix_pos << ". ";
                        //toplot.Insert_samples_1D(shtl, "", "linespoints");
                        //toplot.Insert_samples_1D(shtl, "");

                        //toplot.Set_Global_Title(title.str());
                        //toplot.Plot();
                        //toplot.Plot_as_PDF(Get_Unique_Sequential_Filename("/tmp/pixel_intensity_time_course_",6,".pdf"));
                        //const auto gnuplotfile = toplot.Dump_as_String();
                        //if(!WriteStringToFile(gnuplotfile, Get_Unique_Sequential_Filename("/tmp/pixel_intensity_time_course_",6,".gnuplot"))){
                        //    FUNCWARN("Unable to write gnuplot file. Use the raw data instead");
                        //}
                        //shtl.Plot(title.str());
                        //shtl.Plot_as_PDF(title.str(),Get_Unique_Sequential_Filename("/tmp/pixel_intensity_time_course_",6,".pdf"));
                        shtl.Write_To_File(Get_Unique_Sequential_Filename("/tmp/pixel_intensity_time_course_",6,".txt"));
                        //bool wasOK = false;
                        //auto shtl2 = NPRLL::Attempt_Auto_Smooth(shtl,&wasOK);
                        //if(wasOK) shtl2.Plot(title.str());


                    //Given the current mouse coordinates, dump the pixel value for [A]ll image sets which spatially overlap.
                    // This routine is useful for debugging problematic pixels, or trying to follow per-pixel calculations.
                    //
                    // NOTE: This routine finds the pixel nearest to the specified voxel in DICOM space. So if the image was
                    //       resampled, you will still be able to track the pixel nearest to the centre. In case only exact
                    //       pixels should be tracked, the row and column numbers are spit out; so just filter out pixel
                    //       coordinates you don't want.
                    //
                    }else if( (thechar == 'a') || (thechar == 'A') ){
                        //Get the *realtime* current mouse coordinates.
                        const sf::Vector2i curr_m_pos = sf::Mouse::getPosition(window);

                        //Convert to SFML/OpenGL "World" coordinates. 
                        sf::Vector2f curr_m_pos_w = window.mapPixelToCoords(curr_m_pos);

                        //Check if the mouse is within the image bounding box. We can only proceed if it is.
                        sf::FloatRect disp_img_bbox = disp_img_texture_sprite.second.getGlobalBounds();
                        if(!disp_img_bbox.contains(curr_m_pos_w)){
                            FUNCWARN("The mouse is not currently hovering over the image. Cannot dump time course");
                            break;
                        }

                        //Assuming the image is not rotated or skewed (though possibly scaled), determine which image pixel
                        // we are hovering over.
                        const auto clamped_col_as_f = fabs(curr_m_pos_w.x - disp_img_bbox.left)/disp_img_bbox.width;
                        const auto clamped_row_as_f = fabs(disp_img_bbox.top - curr_m_pos_w.y )/disp_img_bbox.height;

                        const auto img_w_h = disp_img_texture_sprite.first.getSize();
                        const auto col_as_u = static_cast<uint32_t>(clamped_col_as_f * static_cast<float>(img_w_h.x));
                        const auto row_as_u = static_cast<uint32_t>(clamped_row_as_f * static_cast<float>(img_w_h.y));

                        //Get the YgorImage (DICOM) pixel coordinates.
                        const auto pix_pos = disp_img_it->position(row_as_u,col_as_u);
                        const auto ortho = disp_img_it->row_unit.Cross( disp_img_it->col_unit ).unit();
                        const std::list<vec3<double>> points = { pix_pos, pix_pos + ortho * disp_img_it->pxl_dz * 0.25,
                                                                          pix_pos - ortho * disp_img_it->pxl_dz * 0.25 };

                        const auto FOname = Get_Unique_Sequential_Filename("/tmp/pixel_intensity_from_all_overlapping_images_", 6, ".csv");
                        std::fstream FO(FOname, std::fstream::out);
                        if(!FO) FUNCERR("Unable to write to the file '" << FOname << "'. Cannot continue");

                        //Metadata quantities to also harvest.
                        std::vector<std::string> quantities_d = { "dt", "FlipAngle" };
                        std::vector<std::string> quantities_s = { "Description" };

                        FO << "# Image Array Number, Row, Column, Channel, Pixel Value, ";
                        for(auto quantity : quantities_d) FO << quantity << ", ";
                        for(auto quantity : quantities_s) FO << quantity << ", ";
                        FO << std::endl;

                        //Get a list of images which spatially overlap this point. Order should be maintained.
                        size_t ImageArrayNumber = 0;
                        for(auto l_img_array_ptr_it = img_array_ptr_beg; l_img_array_ptr_it != img_array_ptr_end; ++l_img_array_ptr_it, ++ImageArrayNumber){
                            auto encompassing_images = (*l_img_array_ptr_it)->imagecoll.get_images_which_encompass_all_points(points);
                            for(const auto &enc_img_it : encompassing_images){
                                //Find the pixel of interest.
                                for(auto l_chnl = 0; l_chnl < enc_img_it->channels; ++l_chnl){
                                    long int l_row, l_col;
                                    double pix_val;
                                    try{
                                        const auto indx = enc_img_it->index(pix_pos, l_chnl);
                                        if(indx < 0) continue;
                                    
                                        auto rcc = enc_img_it->row_column_channel_from_index(indx);
                                        l_row = std::get<0>(rcc);
                                        l_col = std::get<1>(rcc);
                                        if(l_chnl != std::get<2>(rcc)) continue;

                                        pix_val = enc_img_it->value(l_row, l_col, l_chnl);

                                    }catch(const std::exception &e){
                                        continue;
                                    }
                                    FO << ImageArrayNumber << ", ";
                                    FO << l_row << ", " << l_col << ", " << l_chnl << ", ";
                                    FO << pix_val << ", ";

                                    for(auto quantity : quantities_d){
                                        if(auto q = enc_img_it->GetMetadataValueAs<double>(quantity)){
                                            FO << q.value() << ", ";
                                        }
                                    }
                                    for(auto quantity : quantities_s){
                                        if(auto q = enc_img_it->GetMetadataValueAs<std::string>(quantity)){
                                            FO << Quote_Static_for_Bash(q.value()) << ", ";
                                        }
                                    }
                                    FO << std::endl;

                                }
                            }
                        }
                        FO.close();
                        FUNCINFO("Dumped pixel values which coincide with the specified voxel at"
                                 " row,col = " << row_as_u << "," << col_as_u);


                   //Advance to the next/previous Image_Array. Also reset necessary display image iterators.
                   }else if( (thechar == 'N') || (thechar == 'P') ){
                        //Save the current image position. We will attempt to find the same spot after switching arrays.
                        const auto disp_img_pos = static_cast<size_t>( std::distance(disp_img_beg, disp_img_it) );

                        if(thechar == 'N'){
                            if(img_array_ptr_it == img_array_ptr_last){ //Wrap around forwards.
                                img_array_ptr_it = img_array_ptr_beg;
                            }else{
                                std::advance(img_array_ptr_it, 1);
                            }

                        }else if(thechar == 'P'){
                            if(img_array_ptr_it == img_array_ptr_beg){ //Wrap around backwards.
                                img_array_ptr_it = img_array_ptr_last;
                            }else{
                                std::advance(img_array_ptr_it,-1);
                            }
                        }
                        FUNCINFO("There are " << (*img_array_ptr_it)->imagecoll.images.size() << " images in this Image_Array");

                        disp_img_beg  = (*img_array_ptr_it)->imagecoll.images.begin();
                        disp_img_last = std::prev((*img_array_ptr_it)->imagecoll.images.end());
                        disp_img_it   = disp_img_beg;

                        if(disp_img_pos < (*img_array_ptr_it)->imagecoll.images.size()){
                            std::advance(disp_img_it, disp_img_pos);
                        }

                        if(!contour_coll_shtl.contours.back().points.empty()){
                            contour_coll_shtl.contours.emplace_back();
                            contour_coll_shtl.contours.back().closed = true;
                        }
                        disp_pixel_contours.clear();

                        if(load_img_texture_sprite(disp_img_it, disp_img_texture_sprite)){
                            scale_sprite_to_fill_screen(window,disp_img_it,disp_img_texture_sprite);

                            //const auto img_number = std::distance((*img_array_ptr_it)->imagecoll.images.begin(), disp_img_it);
                            //const auto img_number = std::distance(disp_img_beg, disp_img_it);
                            //FUNCINFO("Loaded next Image_Array. Displaying image number " << img_number);
                            FUNCINFO("Loaded Image_Array " << std::distance(img_array_ptr_beg,img_array_ptr_it) << ". "
                                     "There are " << (*img_array_ptr_it)->imagecoll.images.size() << " images in this Image_Array");
                            
                        }else{ 
                            FUNCERR("Unable to load image --> texture --> sprite");
                        }   

                        if(auto ImageDesc = disp_img_it->GetMetadataValueAs<std::string>("Description")){
                            window.setTitle("DICOMautomaton IV: '"_s + ImageDesc.value() + "'");
                        }else{
                            window.setTitle("DICOMautomaton IV: <no description available>");
                        }

                    //Advance to the next/previous display image in the current Image_Array.
                    }else if( (thechar == 'n') || (thechar == 'p') ){
                        if(thechar == 'n'){
                            if(disp_img_it == disp_img_last){
                                disp_img_it = disp_img_beg;
                            }else{
                                std::advance(disp_img_it, 1);
                            }
                        }else if(thechar == 'p'){
                            if(disp_img_it == disp_img_beg){
                                disp_img_it = disp_img_last;
                            }else{
                                std::advance(disp_img_it,-1);
                            }
                        }

                        if(!contour_coll_shtl.contours.back().points.empty()){
                            contour_coll_shtl.contours.emplace_back();       
                            contour_coll_shtl.contours.back().closed = true;
                        }
                        disp_pixel_contours.clear();

                        if(load_img_texture_sprite(disp_img_it, disp_img_texture_sprite)){
                            scale_sprite_to_fill_screen(window,disp_img_it,disp_img_texture_sprite);

                            //const auto img_number = std::distance((*img_array_ptr_it)->imagecoll.images.begin(), disp_img_it);
                            const auto img_number = std::distance(disp_img_beg, disp_img_it);
                            FUNCINFO("Loaded next texture in unaltered Image_Array order. Displaying image number " << img_number);
                            
                        }else{
                            FUNCERR("Unable to load image --> texture --> sprite");
                        }
 
                        if(auto ImageDesc = disp_img_it->GetMetadataValueAs<std::string>("Description")){
                            window.setTitle("DICOMautomaton IV: '"_s + ImageDesc.value() + "'");
                        }else{
                            window.setTitle("DICOMautomaton IV: <no description available>");
                        }

                        //Scale the image to fill the available space.
                        scale_sprite_to_fill_screen(window,disp_img_it,disp_img_texture_sprite);

                    //Step to the next/previous image which spatially overlaps with the current display image.
                    }else if( (thechar == '-') || (thechar == '+') ){
                        //Get a list of images which spatially overlap this point. Order should be maintained.
                        const auto disp_img_pos = disp_img_it->center();
                        //Simple way. 
                        //auto encompassing_images = (*img_array_ptr_it)->imagecoll.get_images_which_encompass_point(disp_img_pos);
                        //More reliable way.
                        const auto ortho = disp_img_it->row_unit.Cross( disp_img_it->col_unit ).unit();
                        const std::list<vec3<double>> points = { disp_img_pos, disp_img_pos + ortho * disp_img_it->pxl_dz * 0.25,
                                                                               disp_img_pos - ortho * disp_img_it->pxl_dz * 0.25 };
                        auto encompassing_images = (*img_array_ptr_it)->imagecoll.get_images_which_encompass_all_points(points);

                        //Find the images neighbouring the current image.
                        auto enc_img_it = encompassing_images.begin();
                        for(  ; enc_img_it != encompassing_images.end(); ++enc_img_it){
                            if(*enc_img_it == disp_img_it) break;
                        }
                        if(enc_img_it == encompassing_images.end()){
                            FUNCWARN("Unable to step over spatially overlapping images. None found");
                        }else{
                            if(thechar == '-'){
                                if(enc_img_it == encompassing_images.begin()){
                                    disp_img_it = encompassing_images.back();
                                }else{
                                    --enc_img_it;
                                    disp_img_it = *enc_img_it;
                                }
                            }else if(thechar == '+'){
                                ++enc_img_it;
                                if(enc_img_it == encompassing_images.end()){
                                    disp_img_it = encompassing_images.front();
                                }else{
                                    disp_img_it = *enc_img_it;
                                }
                            }
                        }

                        if(load_img_texture_sprite(disp_img_it, disp_img_texture_sprite)){
                            scale_sprite_to_fill_screen(window,disp_img_it,disp_img_texture_sprite);

                            //const auto img_number = std::distance((*img_array_ptr_it)->imagecoll.images.begin(), disp_img_it);
                            const auto img_number = std::distance(disp_img_beg, disp_img_it);
                            FUNCINFO("Loaded next/previous spatially-overlapping texture. Displaying image number " << img_number);

                        }else{
                            FUNCERR("Unable to load image --> texture --> sprite");
                        }

                        if(auto ImageDesc = disp_img_it->GetMetadataValueAs<std::string>("Description")){
                            window.setTitle("DICOMautomaton IV: '"_s + ImageDesc.value() + "'");
                        }else{
                            window.setTitle("DICOMautomaton IV: <no description available>");
                        }
                       
                        //Scale the image to fill the available space.
                        scale_sprite_to_fill_screen(window,disp_img_it,disp_img_texture_sprite);


                    //Reset the image scale to be pixel-for-pixel what is seen on screen. (Unless there is a view
                    // that has some transformation over on-screen objects.)
                    }else if( (thechar == 'l') || (thechar == 'L')){
                        disp_img_texture_sprite.second.setScale(1.0f,1.0f);

                    //Toggle showing metadata tags that are identical to the neighbouring image's metadata tags.
                    }else if( (thechar == 'u') || (thechar == 'U') ){
                        OnlyShowTagsDifferentToNeighbours = !OnlyShowTagsDifferentToNeighbours;

                    //Erase or Empty the current working contour buffer. 
                    }else if( (thechar == 'e') || (thechar == 'E') ){

                        try{
                            const std::string erase_roi = Detox_String(Execute_Command_In_Pipe("zenity --question --text='Erase working ROI?' 2>/dev/null && echo 1"));
                            if(erase_roi != "1"){
                                FUNCINFO("Not erasing contours. Here it is for inspection purposes:" << contour_coll_shtl.write_to_string());
                                throw std::runtime_error("Instructed not to save.");
                            }

                            //Clear the data in preparation for the next contour collection.
                            contour_coll_shtl.contours.clear();
                            contour_coll_shtl.contours.emplace_back();
                            contour_coll_shtl.contours.back().closed = true;
                            disp_pixel_contours.clear();

                            FUNCINFO("Contour collection cleared from working buffer");
                        }catch(const std::exception &){ }

                    //Save the current contour collection.
                    }else if( (thechar == 's') || (thechar == 'S') ){

                        try{
                            auto FrameofReferenceUID = disp_img_it->GetMetadataValueAs<std::string>("FrameofReferenceUID");
                            auto StudyInstanceUID    = disp_img_it->GetMetadataValueAs<std::string>("StudyInstanceUID");
                            if(!FrameofReferenceUID || !StudyInstanceUID) throw std::runtime_error("Missing needed image metadata.");

                            const std::string save_roi = Detox_String(Execute_Command_In_Pipe("zenity --question --text='Save ROI?' 2>/dev/null && echo 1"));
                            if(save_roi != "1"){
                                FUNCINFO("Not saving contours. Here it is for inspection purposes:" << contour_coll_shtl.write_to_string());
                                throw std::runtime_error("Instructed not to save.");
                            }

                            const std::string roi_name = Detox_String(Execute_Command_In_Pipe("zenity --entry --text='What is the name of the ROI?' 2>/dev/null"));
                            if(roi_name.empty()) throw std::runtime_error("Cannot save with an empty ROI name. (Punctuation is removed.)");

                            //Trim empty contours from the shuttle.
                            contour_coll_shtl.Purge_Contours_Below_Point_Count_Threshold(3);
                            if(contour_coll_shtl.contours.empty()) throw std::runtime_error("Given empty contour collection. Contours need >3 points each.");
                            const std::string cc_as_str( contour_coll_shtl.write_to_string() );                             

                            //Attempt to save to the database.
                            pqxx::connection c(db_params);
                            pqxx::work txn(c);

                            std::stringstream ss;
                            ss << "INSERT INTO contours ";
                            ss << "    (ROIName, ContourCollectionString, StudyInstanceUID, FrameofReferenceUID) ";
                            ss << "VALUES ";
                            ss << "    (" << txn.quote(roi_name);
                            ss << "    ," << txn.quote(cc_as_str);
                            ss << "    ," << txn.quote(StudyInstanceUID.value());
                            ss << "    ," << txn.quote(FrameofReferenceUID.value());
                            ss << "    ) ";   // TODO: Insert contour_collection's metadata?
                            ss << "RETURNING ROIName;";

                            FUNCINFO("Executing query:\n\t" << ss.str());
                            pqxx::result res = txn.exec(ss.str());
                            if(res.empty()) throw std::runtime_error("Should have received an ROIName but didn't.");
                            txn.commit();

                            //Clear the data in preparation for the next contour collection.
                            contour_coll_shtl.contours.clear();
                            contour_coll_shtl.contours.emplace_back();
                            contour_coll_shtl.contours.back().closed = true;
                            disp_pixel_contours.clear();

                            FUNCINFO("Contour collection saved to db and cleared");
                        }catch(const std::exception &e){
                            FUNCWARN("Unable to push contour collection to db: '" << e.what() << "'");
                        }

                    }else{
                        FUNCINFO("Character '" << thechar << "' is not yet bound to any action");
                    }

                }else if(window.hasFocus() && (event.type == sf::Event::MouseWheelMoved)){
                    if(VERBOSE && !QUIET){
                        FUNCINFO("Mouse wheel moved");
                        std::cout << "wheel movement: " << event.mouseWheel.delta << std::endl;
                        std::cout << "mouse x: " << event.mouseWheel.x << std::endl;
                        std::cout << "mouse y: " << event.mouseWheel.y << std::endl;
                    }

                }else if(window.hasFocus() && (event.type == sf::Event::MouseButtonPressed)){
                    if(VERBOSE && !QUIET) FUNCINFO("Mouse button pressed");

                    if(event.mouseButton.button == sf::Mouse::Left){
                        if(VERBOSE && !QUIET){
                            std::cout << "the right button was pressed" << std::endl;
                            std::cout << "mouse x: " << event.mouseButton.x << std::endl;
                            std::cout << "mouse y: " << event.mouseButton.y << std::endl;
                        }

                        sf::Vector2f ClickWorldPos = window.mapPixelToCoords(sf::Vector2i(event.mouseButton.x,event.mouseButton.y));
                        sf::FloatRect DispImgBBox = disp_img_texture_sprite.second.getGlobalBounds();
                        if(DispImgBBox.contains(ClickWorldPos)){
                            // ---- Draw on the image where we have clicked ----
                            if(VERBOSE && !QUIET) FUNCINFO("Clicked INSIDE img bbox");
                            //sf::Vector2f DispImgWorldPos = disp_img_texture_sprite.second.getPosition();
                            //const auto left   = DispImgBBox.left;
                            //const auto top    = DispImgBBox.top; 
                            //const auto width  = DispImgBBox.width;
                            //const auto height = DispImgBBox.height;

                            //Assuming the image is not rotated or skewed (though possibly scaled), determine which image pixel
                            // we are hovering over.
                            const auto clamped_col_as_f = fabs(ClickWorldPos.x - DispImgBBox.left)/(DispImgBBox.width);
                            const auto clamped_row_as_f = fabs(DispImgBBox.top - ClickWorldPos.y )/(DispImgBBox.height);

                            const auto img_w_h = disp_img_texture_sprite.first.getSize();
                            const auto col_as_u = static_cast<uint32_t>(clamped_col_as_f * static_cast<float>(img_w_h.x));
                            const auto row_as_u = static_cast<uint32_t>(clamped_row_as_f * static_cast<float>(img_w_h.y));

                            if(VERBOSE && !QUIET) FUNCINFO("Suspected updated row, col = " << row_as_u << ", " << col_as_u);                           
                            sf::Uint8 newpixvals[4] = {255, 0, 0, 255};
                            disp_img_texture_sprite.first.update(newpixvals,1,1,col_as_u,row_as_u);
                            disp_img_texture_sprite.second.setTexture(disp_img_texture_sprite.first);

                            const auto dicom_pos = disp_img_it->position(row_as_u, col_as_u); 
                            auto FrameofReferenceUID = disp_img_it->GetMetadataValueAs<std::string>("FrameofReferenceUID");
                            if(FrameofReferenceUID){
                                //Record the point in the working contour buffer.
                                contour_coll_shtl.contours.back().closed = true;
                                contour_coll_shtl.contours.back().points.push_back( dicom_pos );
                                contour_coll_shtl.contours.back().metadata["FrameofReferenceUID"] = FrameofReferenceUID.value();
                                //const std::string as_str( contour_coll_shtl.contours.back().write_to_string() );
                                //std::cout << "current contour: " << std::endl << as_str << std::endl;
                                //OverwriteStringToFile(as_str, "/tmp/contour_of_points_SliceLocation_"_s + SliceLocation_str);

                                //Record the point in the display contour buffer.
                                disp_pixel_contours.push_back( ClickWorldPos ); //Needs to be in SFML world coordinates.
                            }else{
                                FUNCWARN("Unable to find display image's FrameofReferenceUID. Cannot insert point in contour");

                            }
               
                        }else{
                            if(VERBOSE && !QUIET) FUNCINFO("Clicked OUTSIDE img bbox");
                        }

                    }

                }else if(window.hasFocus() && (event.type == sf::Event::MouseButtonReleased)){
                    //FUNCINFO("Mouse button released");


                }else if(window.hasFocus() && (event.type == sf::Event::MouseMoved)){
                    if(VERBOSE && !QUIET){
                        FUNCINFO("Mouse button moved");
                        std::cout << "Mouse position x,y = " << event.mouseMove.x 
                                                      << "," << event.mouseMove.y << std::endl;
                    }

                    cursortext.setPosition(event.mouseMove.x,event.mouseMove.y);
                    //smallcirc.setPosition(event.mouseMove.x,event.mouseMove.y);

                    //Get the *realtime* position, not necessarily same as the event position. 
                    //sf::Vector2i localPosition = sf::Mouse::getPosition(window);

                    //Print the world coordinates to the console.
                    sf::Vector2f worldPos = window.mapPixelToCoords(sf::Vector2i(event.mouseMove.x,event.mouseMove.y));

                    if(VERBOSE && !QUIET){
                        std::cout << "World Coords x,y = " << worldPos.x
                                                    << "," << worldPos.y << std::endl;
                    }

                    //Display info at the cursor about which image pixel we are on, pixel intensity.
                    sf::Vector2f ClickWorldPos = window.mapPixelToCoords(sf::Vector2i(event.mouseMove.x,event.mouseMove.y));
                    sf::FloatRect DispImgBBox = disp_img_texture_sprite.second.getGlobalBounds();
                    if(DispImgBBox.contains(ClickWorldPos)){
                        //Assuming the image is not rotated or skewed (though possibly scaled), determine which image pixel
                        // we are hovering over.
                        const auto clamped_col_as_f = fabs(ClickWorldPos.x - DispImgBBox.left)/(DispImgBBox.width);
                        const auto clamped_row_as_f = fabs(DispImgBBox.top - ClickWorldPos.y )/(DispImgBBox.height);

                        const auto img_w_h = disp_img_texture_sprite.first.getSize();
                        const auto col_as_u = static_cast<long int>(clamped_col_as_f * static_cast<float>(img_w_h.x));
                        const auto row_as_u = static_cast<long int>(clamped_row_as_f * static_cast<float>(img_w_h.y));

                        if(VERBOSE && !QUIET) FUNCINFO("Suspected updated row, col = " << row_as_u << ", " << col_as_u);
                        const auto pix_val = disp_img_it->value(row_as_u,col_as_u,0);
                        std::stringstream ss;
                        ss << "(r,c)=(" << row_as_u << "," << col_as_u << ") -- " << pix_val;
                        cursortext.setString(ss.str());
                        BLcornertextss.str(""); //Clear stringstream.
                        BLcornertextss << ss.str();
                    }else{
                        cursortext.setString("");
                        BLcornertextss.str(""); //Clear stringstream.
                    }

                }else if(event.type == sf::Event::Resized){
                    if(VERBOSE && !QUIET) FUNCINFO("Window resized to WxH = " << event.size.width << "x" << event.size.height);
                    //window.setSize(sf::Vector2u(event.size.width, event.size.height));
                    sf::View view;

                    //Shrink the image depending on the amount of window space available. The image might disappear off the
                    // screen if the window is too small, but nothing gets squished.
                    view.reset(sf::FloatRect(0, 0, event.size.width, event.size.height));
                    //view.setViewport(sf::FloatRect(0.0f, 0.0f, 1.0f, 1.0f)); //Set target viewport to be LHS of window only.
                    window.setView(view);

                    //Scale the image with the window. Results in highly-squished display, and essentially arbitrary world
                    // coordinate system.
                    //window.setView(window.getDefaultView());
                    scale_sprite_to_fill_screen(window,disp_img_it,disp_img_texture_sprite);

                }else if(event.type == sf::Event::LostFocus){
                }else if(event.type == sf::Event::GainedFocus){
                }else if(event.type == sf::Event::MouseEntered){
                }else if(event.type == sf::Event::MouseLeft){
                }else{
                    FUNCINFO("Ignored event!");
                }
            }

            //Populate the corner text with all non-empty info available.
            if(OnlyShowTagsDifferentToNeighbours 
            && ( (*img_array_ptr_it)->imagecoll.images.size() > 1) ){
                //Get the neighbouring image in the present collection.
                auto next_img_it = (disp_img_it == disp_img_last) ? disp_img_beg : std::next(disp_img_it);

                for(const auto &apair : disp_img_it->metadata){
                    if(apair.second.empty()) continue;
                    const auto matching_pair_it = next_img_it->metadata.find(apair.first);
                    if(matching_pair_it == next_img_it->metadata.end()) continue;
                    if(apair.second == matching_pair_it->second) continue;
                    std::string thekey, theval;
                    if(apair.first.size() < 40){
                         thekey = apair.first;
                    }else{
                         thekey = apair.first.substr(0,30) + "..." + apair.first.substr(apair.first.size() - 7);
                    }
                    if(apair.second.size() < 40){
                         theval = apair.second;
                    }else{
                         theval = apair.second.substr(0,30) + "..." + apair.second.substr(apair.second.size() - 7);
                    }

                    BRcornertextss << thekey << " = " << theval << std::endl;
                }

            }else{
                for(const auto &apair : disp_img_it->metadata){
                    if(apair.second.empty()) continue;
                    std::string thekey, theval;
                    if(apair.first.size() < 40){
                         thekey = apair.first;
                    }else{
                         thekey = apair.first.substr(0,30) + "..." + apair.first.substr(apair.first.size() - 7);
                    }
                    if(apair.second.size() < 40){
                         theval = apair.second;
                    }else{
                         theval = apair.second.substr(0,30) + "..." + apair.second.substr(apair.second.size() - 7);
                    }

                    BRcornertextss << thekey << " = " << theval << std::endl;
                }
            }

            BRcornertextss << "offset = " << disp_img_it->offset << std::endl;
            BRcornertextss << "anchor = " << disp_img_it->anchor << std::endl;
            BRcornertextss << "pxl_dx,dy,dz = " << disp_img_it->pxl_dx << ", " 
                                                << disp_img_it->pxl_dy << ", " 
                                                << disp_img_it->pxl_dz << ", " << std::endl;


            //Begin drawing the window contents.
            window.clear(sf::Color::Black);
            //window.draw(ashape);
            window.draw(smallcirc);

            window.draw(disp_img_texture_sprite.second);

            BRcornertext.setString(BRcornertextss.str());
            BLcornertext.setString(BLcornertextss.str());
            //Move the text to the proper corner.
            {
                const auto item_bbox = BRcornertext.getGlobalBounds();
                const auto item_brc  = sf::Vector2f( item_bbox.left + item_bbox.width, item_bbox.top + item_bbox.height );

                const auto wndw_view = window.getView();
                const auto view_cntr = wndw_view.getCenter();
                const auto view_size = wndw_view.getSize();
                const auto view_brc  = sf::Vector2f( view_cntr.x + 0.48f*view_size.x, view_cntr.y + 0.48f*view_size.y );
     
                //We should have that the item's bottom right corner coincides with the window's bottom right corner.
                const sf::Vector2f offset = view_brc - item_brc;

                //Now move the text over.
                BRcornertext.move(offset);
            }            
            {
                const auto item_bbox = BLcornertext.getGlobalBounds();
                const auto item_blc  = sf::Vector2f( item_bbox.left, item_bbox.top + item_bbox.height );

                const auto wndw_view = window.getView();
                const auto view_cntr = wndw_view.getCenter();
                const auto view_size = wndw_view.getSize();
                const auto view_blc  = sf::Vector2f( view_cntr.x - 0.48f*view_size.x, view_cntr.y + 0.48f*view_size.y );

                //We should have that the item's bottom right corner coincides with the window's bottom right corner.
                const sf::Vector2f offset = view_blc - item_blc;

                //Now move the text over.
                BLcornertext.move(offset);
            }

            window.draw(BRcornertext);
            window.draw(cursortext);
            window.draw(BLcornertext);

            //Draw any contours that lie in the plane of the current image. Also draw contour names if the cursor is 'within' them.
            {
                sf::Text contourtext;
                std::stringstream contourtextss;
                contourtext.setFont(afont);
                contourtext.setString("");
                contourtext.setCharacterSize(12); //Size in pixels, not in points.
                contourtext.setColor(sf::Color::Green);

                for(auto & cc : DICOM_data.contour_data->ccs){
                    //auto base_ptr = reinterpret_cast<contour_collection<double> *>(&cc);
                    for(auto & c : cc.contours){
                        if(disp_img_it->encompasses_contour_of_points(c)){
                            sf::VertexArray lines;
                            lines.setPrimitiveType(sf::LinesStrip);

                            for(auto & p : c.points){
                                //We have three distinct coordinate systems: DICOM, pixel coordinates and screen pixel coordinates,
                                // and SFML 'world' coordinates. We need to map from the DICOM coordinates to screen pixel coords.
                                const auto img_index = disp_img_it->index(p,0); //Channel = 0.
                                const auto img_rcc = disp_img_it->row_column_channel_from_index(img_index);
                                const auto img_row = std::get<0>(img_rcc);
                                const auto img_col = std::get<1>(img_rcc);
                                const auto clamped_col_as_f = (static_cast<float>(img_col) + 0.5f)/static_cast<float>(disp_img_it->columns);
                                const auto clamped_row_as_f = (static_cast<float>(img_row) + 0.5f)/static_cast<float>(disp_img_it->rows);

                                sf::FloatRect DispImgBBox = disp_img_texture_sprite.second.getGlobalBounds(); //Uses top left corner as (0,0).
                                const auto world_x = DispImgBBox.left + DispImgBBox.width  * clamped_col_as_f;
                                const auto world_y = DispImgBBox.top  + DispImgBBox.height * clamped_row_as_f;

                                lines.append( sf::Vertex( sf::Vector2f(world_x, world_y), sf::Color::Blue ) );
                            }
                            window.draw(lines);

                            //Check if the mouse is within the contour. If so, display the name.
                            const sf::Vector2i mouse_coords = sf::Mouse::getPosition();
                            //--------------------
                            sf::Vector2f mouse_world_pos = window.mapPixelToCoords(mouse_coords);
                            sf::FloatRect DispImgBBox = disp_img_texture_sprite.second.getGlobalBounds();
                            if(DispImgBBox.contains(mouse_world_pos)){
                                //Assuming the image is not rotated or skewed (though possibly scaled), determine which image pixel
                                // we are hovering over.
                                const auto clamped_col_as_f = fabs(mouse_world_pos.x - DispImgBBox.left)/(DispImgBBox.width);
                                const auto clamped_row_as_f = fabs(DispImgBBox.top - mouse_world_pos.y )/(DispImgBBox.height);

                                const auto img_w_h = disp_img_texture_sprite.first.getSize();
                                const auto col_as_u = static_cast<uint32_t>(clamped_col_as_f * static_cast<float>(img_w_h.x));
                                const auto row_as_u = static_cast<uint32_t>(clamped_row_as_f * static_cast<float>(img_w_h.y));
                                const auto dicom_pos = disp_img_it->position(row_as_u, col_as_u);

                                const auto img_plane = disp_img_it->image_plane();
                                if(c.Is_Point_In_Polygon_Projected_Orthogonally(img_plane,dicom_pos)){
                                    auto ROINameOpt = c.GetMetadataValueAs<std::string>("ROIName");
                                    auto NormROINameOpt = c.GetMetadataValueAs<std::string>("NormalizedROIName");
                                    contourtextss << (NormROINameOpt ? NormROINameOpt.value() : "???");
                                    contourtextss << " --- "; 
                                    contourtextss << (ROINameOpt ? ROINameOpt.value() : "???");
                                    contourtextss << std::endl; 
                                }
                            //--------------------
                            }
                        }
                    }
                }

                contourtext.setString(contourtextss.str());
                const auto item_bbox = contourtext.getGlobalBounds();
                const auto item_trc  = sf::Vector2f( item_bbox.left + item_bbox.width, item_bbox.top );

                const auto wndw_view = window.getView();
                const auto view_cntr = wndw_view.getCenter();
                const auto view_size = wndw_view.getSize();
                const auto view_trc  = sf::Vector2f( view_cntr.x + 0.48f*view_size.x, view_cntr.y - 0.48f*view_size.y );

                //We should have that the item's bottom right corner coincides with the window's bottom right corner.
                const sf::Vector2f offset = view_trc - item_trc;

                //Now move the text over.
                contourtext.move(offset);
                window.draw(contourtext);
            }


            //Draw the being-drawn contours in native screen-pixel coordinates, if there are any.
            // NOTE: Don't use this routine because it is less robust than using DICOM coordinates.
            //if(!disp_pixel_contours.empty()){
            //    sf::VertexArray lines;
            //    lines.setPrimitiveType(sf::LinesStrip);
            //    
            //    for(const auto &vert : disp_pixel_contours){
            //        lines.append( sf::Vertex(vert, sf::Color::Yellow) );
            //    }
            //    window.draw(lines);
            //}


            //Draw any contours from the contouring buffer that lie in the plane of the current image.
            {
                for(auto & c : contour_coll_shtl.contours){
                    if(!c.points.empty() && disp_img_it->encompasses_contour_of_points(c)){
                        sf::VertexArray lines;
                        lines.setPrimitiveType(sf::LinesStrip);

                        for(auto & p : c.points){
                            //We have three distinct coordinate systems: DICOM, pixel coordinates and screen pixel coordinates,
                            // and SFML 'world' coordinates. We need to map from the DICOM coordinates to screen pixel coords.
                            const auto img_index = disp_img_it->index(p,0); //Channel = 0.
                            const auto img_rcc = disp_img_it->row_column_channel_from_index(img_index);
                            const auto img_row = std::get<0>(img_rcc);
                            const auto img_col = std::get<1>(img_rcc);
                            const auto clamped_col_as_f = (static_cast<float>(img_col) + 0.5f)/static_cast<float>(disp_img_it->columns);
                            const auto clamped_row_as_f = (static_cast<float>(img_row) + 0.5f)/static_cast<float>(disp_img_it->rows);

                            sf::FloatRect DispImgBBox = disp_img_texture_sprite.second.getGlobalBounds(); //Uses top left corner as (0,0).
                            const auto world_x = DispImgBBox.left + DispImgBBox.width  * clamped_col_as_f;
                            const auto world_y = DispImgBBox.top  + DispImgBBox.height * clamped_row_as_f;

                            lines.append( sf::Vertex( sf::Vector2f(world_x, world_y), sf::Color::Magenta ) );
                        }
                        window.draw(lines);
                    }
                }
            }


            window.display();

            if(DumpScreenshot){
                DumpScreenshot = false;
                const auto fname_sshot = Get_Unique_Sequential_Filename("/tmp/DICOMautomaton_screenshot_",6,".png");
                if(!window.capture().saveToFile(fname_sshot)){
                    FUNCWARN("Unable to dump screenshot to file '" << fname_sshot << "'");
                }
            }

        }
    }


//---------------------------------------------------------------------------------------------------------------------
//---------------------------------------------------- Cleanup --------------------------------------------------------
//---------------------------------------------------------------------------------------------------------------------
    return 0;
}
