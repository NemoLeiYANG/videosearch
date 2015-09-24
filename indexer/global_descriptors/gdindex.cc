#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <stdio.h>
#include <string>
#include <vector>

#include "../../common/feature_set/feature_set.h"
#include "gdindex.h"

extern "C" {
#include "../../common/yael_v438_modif/yael/gmm.h"
#include "../../common/yael_v438_modif/yael/matrix.h"
}

GDIndex::GDIndex() {
    // Initializing member variables to default values
    // -- Index
    index_.number_global_descriptors = 0;
    index_.word_descriptor.clear();
    index_.word_l1_norms.clear();
    index_.word_total_soft_assignment.clear();
    index_.frame_numbers_in_db.clear();
    // -- Index parameters
    index_parameters_.ld_length = LD_LENGTH_DEFAULT;
    index_parameters_.ld_frame_length = LD_FRAME_LENGTH_DEFAULT;
    index_parameters_.ld_extension = LD_EXTENSION_DEFAULT;
    index_parameters_.ld_name = LD_NAME_DEFAULT;
    index_parameters_.ld_pca_dim = LD_PCA_DIM_DEFAULT;
    index_parameters_.ld_pre_pca_power = LD_PRE_PCA_POWER_DEFAULT;
    index_parameters_.ld_mean_vector = NULL;
    index_parameters_.ld_pca_eigenvectors.clear();
    index_parameters_.gd_gmm = NULL;
    index_parameters_.gd_number_gaussians = GD_NUMBER_GAUSSIANS_DEFAULT;
    index_parameters_.gd_power = GD_POWER_DEFAULT;
    // -- Query parameters
    query_parameters_.min_number_words_visited = MIN_NUMBER_WORDS_VISITED_DEFAULT;
    query_parameters_.word_selection_mode = WORD_SELECTION_MODE_DEFAULT;
    query_parameters_.word_selection_thresh = WORD_SELECTION_THRESH_DEFAULT;
    query_parameters_.fast_corr_weights = NULL;
}

GDIndex::~GDIndex() {
    // Clearing and deleting vectors/pointers
    // -- Index
    index_.word_descriptor.clear();
    index_.word_l1_norms.clear();
    index_.word_total_soft_assignment.clear();
    index_.frame_numbers_in_db.clear();
    // -- Index parameters
    if (index_parameters_.ld_mean_vector != NULL) {
        delete[] index_parameters_.ld_mean_vector;
        index_parameters_.ld_mean_vector = NULL;
    }
    for (uint i = 0; i < index_parameters_.ld_pca_eigenvectors.size(); i++) {
        if (index_parameters_.ld_pca_eigenvectors.at(i) != NULL) {
            delete [] index_parameters_.ld_pca_eigenvectors.at(i);
            index_parameters_.ld_pca_eigenvectors.at(i) = NULL;
        }
    }
    index_parameters_.ld_pca_eigenvectors.clear();
    if (index_parameters_.gd_gmm != NULL) {
        gmm_delete(index_parameters_.gd_gmm);
    }
    // -- Query parameters
    if (query_parameters_.fast_corr_weights != NULL) {
        delete [] query_parameters_.fast_corr_weights;
        query_parameters_.fast_corr_weights = NULL;
    }
}

void GDIndex::write(const string index_path) {
    int n_write; // aux. variable for writing

    // Open file for writing
    FILE* index_file = fopen(index_path.c_str(), "wb");
    if (index_file == NULL) {
        fprintf(stderr, "GDIndex::write : Cannot open: %s\n", index_path.c_str());
        exit(EXIT_FAILURE);
    }

    // Write number of global descriptors
    int number_gd_to_write = static_cast<int>(index_.number_global_descriptors);
    n_write = fwrite(&number_gd_to_write, sizeof(int), 1, index_file);
    
    // Assert that we have data in index_ and that it makes sense, 
    // and allocate helper variables
    assert(index_.word_descriptor.size() != 0);
    assert(index_.word_l1_norms.size() != 0);
    assert(index_.word_total_soft_assignment.size() != 0);
    assert(index_.word_descriptor.size() == index_.word_l1_norms.size());
    assert(index_.word_descriptor.size() == index_.word_total_soft_assignment.size());
    uint* word_descriptor_to_write = 
        new uint[index_parameters_.gd_number_gaussians];
    float* word_l1_norms_to_write = 
        new float[index_parameters_.gd_number_gaussians];
    float* word_total_soft_assignment_to_write = 
        new float[index_parameters_.gd_number_gaussians];

    // Loop over items in index and write them out
    for (int count_item = 0; count_item < number_gd_to_write; count_item++) {
        // Collect data that will be written
        for (uint count_gaussian = 0; 
             count_gaussian < index_parameters_.gd_number_gaussians; 
             count_gaussian++) {
            word_l1_norms_to_write[count_gaussian] = 
                index_.word_l1_norms.at(count_item).at(count_gaussian);
            word_total_soft_assignment_to_write[count_gaussian] = 
                index_.word_total_soft_assignment.at(count_item).at(count_gaussian);
            word_descriptor_to_write[count_gaussian] = 
                index_.word_descriptor.at(count_item).at(count_gaussian);
        }
        // Write to file
        n_write = fwrite(word_l1_norms_to_write, sizeof(float), 
                         index_parameters_.gd_number_gaussians, index_file);
        n_write = fwrite(word_total_soft_assignment_to_write, sizeof(float), 
                         index_parameters_.gd_number_gaussians, index_file);
        n_write = fwrite(word_descriptor_to_write, sizeof(uint), 
                         index_parameters_.gd_number_gaussians, index_file);
    }

    // Clean up
    if (word_l1_norms_to_write != NULL) {
        delete [] word_l1_norms_to_write;
        word_l1_norms_to_write = NULL;
    }
    if (word_total_soft_assignment_to_write != NULL) {
        delete [] word_total_soft_assignment_to_write;
        word_total_soft_assignment_to_write = NULL;
    }
    if (word_descriptor_to_write != NULL) {
        delete [] word_descriptor_to_write;
        word_descriptor_to_write = NULL;
    }

    // Close file
    fclose(index_file);
}

void GDIndex::read(const string index_path) {
    int n_read; // aux. variable for reading

    // Open file for reading
    FILE* index_file = fopen(index_path.c_str(), "rb");
    if (index_file == NULL) {
        fprintf(stderr, "GDIndex::read : Cannot open: %s\n", index_path.c_str());
        exit(EXIT_FAILURE);
    }

    // Read number of global descriptors
    int number_gd_to_read = 0;
    n_read = fread(&number_gd_to_read, sizeof(int), 1, index_file);

    // Size of current index
    int current_db_size = index_.number_global_descriptors;

    // Allocate helper variables
    uint* word_descriptor_to_read = 
        new uint[index_parameters_.gd_number_gaussians];
    float* word_l1_norms_to_read = 
        new float[index_parameters_.gd_number_gaussians];
    float* word_total_soft_assignment_to_read = 
        new float[index_parameters_.gd_number_gaussians];

    // Depending on query_parameters_.word_selection_mode, we will load
    // either l1 norms of total soft assignment information. So the resize
    // of index_ vectors is done only to one or the other
    index_.word_descriptor.resize(current_db_size + number_gd_to_read);
    if (query_parameters_.word_selection_mode == WORD_L1_NORM) {
        index_.word_l1_norms.resize(current_db_size + number_gd_to_read);
    } else if (query_parameters_.word_selection_mode == WORD_SOFT_ASSGN) {
        index_.word_total_soft_assignment.resize(current_db_size + number_gd_to_read);
    } else {
        cout << "Error! Mode " << query_parameters_.word_selection_mode 
             << " is not allowed. Quitting..."
             << endl;
        exit(EXIT_FAILURE);
    }
    
    // Loop over items, read and insert them into index_
    for (int count_item = current_db_size; 
         count_item < current_db_size + number_gd_to_read; 
         count_item++) {
        // Read data
        n_read = fread(word_l1_norms_to_read, sizeof(float), 
                       index_parameters_.gd_number_gaussians, index_file);
        n_read = fread(word_total_soft_assignment_to_read, sizeof(float), 
                       index_parameters_.gd_number_gaussians, index_file);
        n_read = fread(word_descriptor_to_read, sizeof(uint), 
                       index_parameters_.gd_number_gaussians, index_file);

        // Insert data into index_
        if (query_parameters_.word_selection_mode == WORD_L1_NORM) {
            index_.word_l1_norms.at(count_item)
                .resize(index_parameters_.gd_number_gaussians);
            for (uint count_gaussian = 0; 
                 count_gaussian < index_parameters_.gd_number_gaussians; 
                 count_gaussian++) {
                index_.word_l1_norms.at(count_item).at(count_gaussian)
                    = word_l1_norms_to_read[count_gaussian];
            }
        } else {
            index_.word_total_soft_assignment.at(count_item)
                .resize(index_parameters_.gd_number_gaussians);
            for (uint count_gaussian = 0; 
                 count_gaussian < index_parameters_.gd_number_gaussians; 
                 count_gaussian++) {
                index_.word_total_soft_assignment.at(count_item).at(count_gaussian)
                    = word_total_soft_assignment_to_read[count_gaussian];
            }
        }
        index_.word_descriptor.at(count_item)
            .resize(index_parameters_.gd_number_gaussians);
        for (uint count_gaussian = 0; 
             count_gaussian < index_parameters_.gd_number_gaussians; 
             count_gaussian++) {
            index_.word_descriptor.at(count_item).at(count_gaussian)
                = word_descriptor_to_read[count_gaussian];
        }
    }

    // Clean up
    if (word_l1_norms_to_read != NULL) {
        delete [] word_l1_norms_to_read;
        word_l1_norms_to_read = NULL;
    }
    if (word_total_soft_assignment_to_read != NULL) {
        delete [] word_total_soft_assignment_to_read;
        word_total_soft_assignment_to_read = NULL;
    }
    if (word_descriptor_to_read != NULL) {
        delete [] word_descriptor_to_read;
        word_descriptor_to_read = NULL;
    }

    // Close file
    fclose(index_file);

    // Update other index_ variables, since now index has changed
    update_index();
}

void GDIndex::write_frame_list(const string file_path) {
    ofstream out_file;
    out_file.open(file_path.c_str());
    uint number_frames_in_db = index_.frame_numbers_in_db.size();
    for (uint count_line = 0; count_line < number_frames_in_db; count_line++) {
        out_file << index_.frame_numbers_in_db.at(count_line) << endl;
    }
    out_file.close();
}

void GDIndex::clean_index() {
    index_.word_descriptor.clear();
    index_.word_l1_norms.clear();
    index_.word_total_soft_assignment.clear();
    index_.frame_numbers_in_db.clear();

    update_index();
}

uint GDIndex::get_number_global_descriptors() {
    return index_.number_global_descriptors;
}

void GDIndex::generate_index(const vector<string>& feature_files, 
                             const int verbose_level) {

}

void GDIndex::generate_index_shot_based(const vector<string>& feature_files, 
                                        const vector<uint>& shot_beg_frames,
                                        const int shot_mode, const int shot_keyf, 
                                        const vector < vector < 
                                            pair < uint, uint > > >& track_lists,
                                        const int verbose_level) {

}

void GDIndex::generate_global_descriptor(const FeatureSet* feature_set, 
                                         vector<uint>& gd_word_descriptor, 
                                         vector<float>& gd_word_l1_norm, 
                                         vector<float>& gd_word_total_soft_assignment) {

}

void GDIndex::performQuery(const string local_descriptors_path, 
                           vector< pair<float,uint> >& results, 
                           const vector<uint>& indices,
                           const uint num_scenes_to_rerank,
                           const uint group_testing_number_centroids ,
                           const GDIndex* revv_other_ptr,
                           const vector < vector < uint > >& vGroupLists,
                           const vector < pair < string, pair < uint, uint > > >& shot_info,
                           const int verbose_level) {

}

void GDIndex::set_index_parameters(const uint ld_length, const uint ld_frame_length,
                                   const string ld_extension, const string ld_name,
                                   const uint ld_pca_dim, const float ld_pre_pca_power,
                                   const uint gd_number_gaussians, const float gd_power,
                                   const string trained_parameters_path,
                                   const int verbose_level) {

}

void GDIndex::set_query_parameters(const uint min_number_words_visited,
                                   const int word_selection_mode,
                                   const float word_selection_thresh,
                                   const string trained_parameters_path,
                                   const int verbose_level) {

}

void GDIndex::update_index() {
    // Update number of global descriptors stored
    index_.number_global_descriptors = index_.word_descriptor.size();

    // Update number of words selected and norm. factors for each db item
    // The purpose of doing this here is to have these numbers ready when
    // querying (and not need to recalculate them at query time)
    index_.number_words_selected.resize(index_.number_global_descriptors, 
                                        index_parameters_.gd_number_gaussians);
    index_.norm_factors.resize(index_.number_global_descriptors, 
                               sqrt(index_parameters_.gd_number_gaussians
                                    * index_parameters_.ld_pca_dim));
    
    // TODO(andrefaraujo): for future work, we might want to include here
    // the possibility of skipping Gaussian residuals on the database
    // side as well
}

void GDIndex::sign_binarize(const vector<float>& gd_word_residuals, 
                            vector<uint>& gd_word_descriptor) {

}

void GDIndex::project_local_descriptr_pca(const float* desc, float* pca_desc) {

}

void GDIndex::sampleFramesFromShot(const uint number_frames_out, 
                                   const uint first_frame, 
                                   const uint number_frames_this_shot, 
                                   vector<uint>& out_frames) {

}

void GDIndex::query(const vector<uint>& query_word_descriptor,
                    const vector<float>& query_word_l1_norm,
                    const vector<float>& query_word_total_soft_assignment,
                    const vector<uint>& database_indices,
                    vector< pair<float,uint> >& database_scores_indices) {

}

void GDIndex::load_ld_mean_vector(string path) {

}

void GDIndex::load_ld_pca_eigenvectors(string path) {

}

void GDIndex::load_gd_gmm(string path) {

}

void GDIndex::load_corr_weights(string path) {

}
