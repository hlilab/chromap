#include "chromap.h"

#include <assert.h>
#include <math.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <type_traits>
#include <unordered_map>

namespace chromap {

void Chromap::ConstructIndex() {
  // TODO(Haowen): Need a faster algorithm
  // Load all sequences in the reference into one batch
  SequenceBatch reference;
  reference.InitializeLoading(index_parameters_.reference_file_path);
  reference.LoadAllSequences();
  const uint32_t num_sequences = reference.GetNumSequences();
  Index index(index_parameters_);
  index.Construct(num_sequences, reference);
  index.Statistics(num_sequences, reference);
  index.Save();
  reference.FinalizeLoading();
}

uint32_t Chromap::LoadSingleEndReadsWithBarcodes(SequenceBatch &read_batch,
                                                 SequenceBatch &barcode_batch,
                                                 bool parallel_parsing) {
  //double real_start_time = GetRealTime();
  uint32_t num_loaded_reads = 0;

  if (!parallel_parsing || mapping_parameters_.is_bulk_data) {
    while (num_loaded_reads < read_batch_size_) {
      bool no_more_read = read_batch.LoadOneSequenceAndSaveAt(num_loaded_reads);
      bool no_more_barcode = no_more_read;
      if (!mapping_parameters_.is_bulk_data) {
        no_more_barcode =
          barcode_batch.LoadOneSequenceAndSaveAt(num_loaded_reads);
      }

      if (no_more_read && no_more_barcode) {
        break;
      } else if (no_more_read || no_more_barcode){
        ExitWithMessage("Numbers of reads and barcodes don't match!");
      }
      ++num_loaded_reads;
    }
  } else {
    uint32_t num_loaded_barcode = 0 ;
    
#pragma omp task shared(num_loaded_reads, read_batch)
    {
      uint32_t i = 0 ;
      for (i = 0 ; i < read_batch_size_; ++i) {
        if (read_batch.LoadOneSequenceAndSaveAt(i) == true) { // true: no more read
          break ;
        }
      }
      num_loaded_reads = i ;
    }

#pragma omp task shared(num_loaded_barcode, barcode_batch)
    { // bulk data will go to the other big branch
      uint32_t i = 0 ;
      for (i = 0 ; i < read_batch_size_; ++i) {
        if (barcode_batch.LoadOneSequenceAndSaveAt(i) == true) { // true: no more read
          break ;
        }
      }
      num_loaded_barcode = i ;
    }

#pragma omp taskwait

    if (num_loaded_reads != num_loaded_barcode) {
        ExitWithMessage("Numbers of reads and barcodes don't match!");
    }
  }
  /*if (num_loaded_reads > 0) {
    std::cerr << "Loaded " << num_loaded_reads << " reads in "
              << GetRealTime() - real_start_time << "s.\n";
  } else {
    std::cerr << "No more reads.\n";
  }*/
  return num_loaded_reads;
}

uint32_t Chromap::LoadPairedEndReadsWithBarcodes(SequenceBatch &read_batch1,
                                                 SequenceBatch &read_batch2,
                                                 SequenceBatch &barcode_batch,
                                                 bool parallel_parsing) {
  // double real_start_time = Chromap<>::GetRealTime();
  uint32_t num_loaded_pairs = 0;
  
  if (!parallel_parsing) {
    while (num_loaded_pairs < read_batch_size_) {
      bool no_more_read1 = read_batch1.LoadOneSequenceAndSaveAt(num_loaded_pairs);
      bool no_more_read2 = read_batch2.LoadOneSequenceAndSaveAt(num_loaded_pairs);
      bool no_more_barcode = no_more_read2;
      if (!mapping_parameters_.is_bulk_data) {
        no_more_barcode =
          barcode_batch.LoadOneSequenceAndSaveAt(num_loaded_pairs);
      }
      
      if (no_more_read1 && no_more_read2 && no_more_barcode) {
        break;
      } else if (no_more_read1 || no_more_read2 || no_more_barcode){
        ExitWithMessage("Numbers of reads and barcodes don't match!");
      }
      ++num_loaded_pairs;
    }
  } else {
    uint32_t num_loaded_read1 = 0;
    uint32_t num_loaded_read2 = 0;
    uint32_t num_loaded_barcode = 0;
    
#pragma omp task shared(num_loaded_read1, read_batch1)
    {
      uint32_t i = 0 ;
      for (i = 0 ; i < read_batch_size_; ++i) {
        if (read_batch1.LoadOneSequenceAndSaveAt(i) == true) { // true: no more read
          break ;
        }
      }
      num_loaded_read1 = i ;
    }

#pragma omp task shared(num_loaded_read2, read_batch2)
    {
      uint32_t i = 0 ;
      for (i = 0 ; i < read_batch_size_; ++i) {
        if (read_batch2.LoadOneSequenceAndSaveAt(i) == true) { // true: no more read
          break ;
        }
      }
      num_loaded_read2 = i ;
    }

#pragma omp task shared(num_loaded_barcode, barcode_batch)
    {
      if (!mapping_parameters_.is_bulk_data) {
        uint32_t i = 0 ;
        for (i = 0 ; i < read_batch_size_; ++i) {
          if (barcode_batch.LoadOneSequenceAndSaveAt(i) == true) { // true: no more read
            break ;
          }
        }
        num_loaded_barcode = i ;
      }
    }

#pragma omp taskwait
    if (mapping_parameters_.is_bulk_data) {
      num_loaded_barcode = num_loaded_read2;
    }
    if (num_loaded_read1 != num_loaded_read2 || num_loaded_read2 != num_loaded_barcode) {
        ExitWithMessage("Numbers of reads and barcodes don't match!");
    }

    num_loaded_pairs = num_loaded_read1 ;
  }
  // if (num_loaded_pairs > 0) {
  //  std::cerr << "Loaded " << num_loaded_pairs << " pairs in "<<
  //  Chromap<>::GetRealTime() - real_start_time << "s. ";
  //} else {
  //  std::cerr << "No more reads.\n";
  //}
  return num_loaded_pairs;
}

void Chromap::TrimAdapterForPairedEndRead(uint32_t pair_index,
                                          SequenceBatch &read_batch1,
                                          SequenceBatch &read_batch2) {
  const uint32_t raw_read1_length = read_batch1.GetSequenceLengthAt(pair_index);
  const uint32_t raw_read2_length = read_batch2.GetSequenceLengthAt(pair_index);
  const char *raw_read1 = read_batch1.GetSequenceAt(pair_index);
  const char *raw_read2 = read_batch2.GetSequenceAt(pair_index);
  const std::string &raw_negative_read1 =
      read_batch1.GetNegativeSequenceAt(pair_index);
  const std::string &raw_negative_read2 =
      read_batch2.GetNegativeSequenceAt(pair_index);

  // In the actual adaptor trimming, we assuem length(read1)<=length(read2). So
  // we can have the case that read1 is a subset of read2.
  const char *read1 =
      raw_read1_length <= raw_read2_length ? raw_read1 : raw_read2;
  const std::string &negative_read2 = raw_read1_length <= raw_read2_length
                                          ? raw_negative_read2
                                          : raw_negative_read1;
  const uint32_t read1_length = raw_read1_length <= raw_read2_length
                                    ? raw_read1_length
                                    : raw_read2_length;
  const uint32_t read2_length = raw_read1_length <= raw_read2_length
                                    ? raw_read2_length
                                    : raw_read1_length;

  const int min_overlap_length = mapping_parameters_.min_read_length;
  const int seed_length = min_overlap_length / 2;
  const int error_threshold_for_merging = 1;
  bool is_merged = false;

  for (int si = 0; si < error_threshold_for_merging + 1; ++si) {
    size_t seed_start_position =
        negative_read2.find(read1 + si * seed_length, 0, seed_length);

    while (seed_start_position != std::string::npos) {
      const bool before_seed_is_enough_long =
          seed_start_position >= (size_t)(si * seed_length);
      const bool overlap_is_enough_long =
          (int)(read2_length - seed_start_position + seed_length * si) >=
          min_overlap_length;

      if (!before_seed_is_enough_long || !overlap_is_enough_long) {
        seed_start_position = negative_read2.find(
            read1 + si * seed_length, seed_start_position + 1, seed_length);
        continue;
      }

      bool can_merge = true;
      int num_errors = 0;

      // The bases before the seed.
      for (int i = 0; i < seed_length * si; ++i) {
        if (negative_read2[seed_start_position - si * seed_length + i] !=
            read1[i]) {
          ++num_errors;
        }
        if (num_errors > error_threshold_for_merging) {
          can_merge = false;
          break;
        }
      }

      // The bases after the seed.
      for (uint32_t i = seed_length; i + seed_start_position < read2_length &&
                                     si * seed_length + i < read1_length;
           ++i) {
        if (negative_read2[seed_start_position + i] !=
            read1[si * seed_length + i]) {
          ++num_errors;
        }
        if (num_errors > error_threshold_for_merging) {
          can_merge = false;
          break;
        }
      }

      if (can_merge) {
        // Trim adapters and TODO: fix sequencing errors
        int overlap_length =
            read2_length - seed_start_position + si * seed_length;
        int read2_offset = 0;
        // The case that read1 is strictly contained in read2. overlap_length is
        // inferred from the longer read2, which could be longer than read1. In
        // that case, we don't trim read1 (make overlap length equal to read1
        // length) and trim read2 as the original plan.
        if (overlap_length > (int)read1_length) {
          read2_offset = overlap_length - read1_length;
          overlap_length = read1_length;
        }

        if (raw_read1_length <= raw_read2_length) {
          read_batch1.TrimSequenceAt(pair_index, overlap_length);
          read_batch2.TrimSequenceAt(pair_index, overlap_length + read2_offset);
        } else {
          read_batch1.TrimSequenceAt(pair_index, overlap_length + read2_offset);
          read_batch2.TrimSequenceAt(pair_index, overlap_length);
        }

        is_merged = true;
        // std::cerr << "Trimed! overlap length: " << overlap_length << ", " <<
        // read1.GetLength() << " " << read2.GetLength() << "\n";
        break;
      }

      seed_start_position = negative_read2.find(
          read1 + si * seed_length, seed_start_position + 1, seed_length);
    }

    if (is_merged) {
      break;
    }
  }
}

bool Chromap::PairedEndReadWithBarcodeIsDuplicate(
    uint32_t pair_index, const SequenceBatch &barcode_batch,
    const SequenceBatch &read_batch1, const SequenceBatch &read_batch2) {
  int dedupe_seed_length = 16;
  uint32_t barcode_length = barcode_batch.GetSequenceLengthAt(pair_index);
  uint64_t barcode_key =
      barcode_batch.GenerateSeedFromSequenceAt(pair_index, 0, barcode_length);
  uint64_t read1_seed1 =
      read_batch1.GenerateSeedFromSequenceAt(pair_index, 0, dedupe_seed_length);
  uint64_t read2_seed1 =
      read_batch2.GenerateSeedFromSequenceAt(pair_index, 0, dedupe_seed_length);
  uint64_t read_seed_key =
      (read1_seed1 << (dedupe_seed_length * 2)) | read2_seed1;
  uint64_t read1_seed2 = read_batch1.GenerateSeedFromSequenceAt(
      pair_index, dedupe_seed_length, dedupe_seed_length * 2);
  uint64_t read2_seed2 = read_batch2.GenerateSeedFromSequenceAt(
      pair_index, dedupe_seed_length, dedupe_seed_length * 2);
  khiter_t barcode_table_iterator =
      kh_get(k64_seq, barcode_lookup_table_, barcode_key);
  if (barcode_table_iterator != kh_end(barcode_lookup_table_)) {
    uint32_t read_lookup_table_index =
        kh_value(barcode_lookup_table_, barcode_table_iterator);
    // std::cerr << "Have barcode, try to check read. " <<
    // read_lookup_table_index << "\n";
    khash_t(k128) *read_lookup_table =
        read_lookup_tables_[read_lookup_table_index];
    khiter_t read_lookup_table_iterator =
        kh_get(k128, read_lookup_table, read_seed_key);
    if (read_lookup_table_iterator != kh_end(read_lookup_table)) {
      // std::cerr << "Have barcode, have read, try whether match.\n";
      uint128_t read_seeds =
          kh_value(read_lookup_table, read_lookup_table_iterator);
      if (read_seeds.first == read1_seed2 && read_seeds.second == read2_seed2) {
        // std::cerr << "Have barcode, have read, and match.\n";
        return true;
      } else {
        // std::cerr << "Have barcode, have read, but don't match.\n";
        return false;
      }
    } else {
      // std::cerr << "Have barcode, no read.\n";
      uint128_t read_seeds = {.first = read1_seed2, .second = read2_seed2};
      int khash_return_code;
      khiter_t read_lookup_table_insert_iterator =
          kh_put(k128, read_lookup_table, read_seed_key, &khash_return_code);
      assert(khash_return_code != -1 && khash_return_code != 0);
      kh_value(read_lookup_table, read_lookup_table_insert_iterator) =
          read_seeds;
      // std::cerr << "Have barcode, no read.\n";
      return false;
    }
  } else {
    // insert the barcode and append a new read hash table to tables and then
    // insert the reads
    // std::cerr << "No barcode, no read.\n";
    int khash_return_code;
    khiter_t barcode_table_insert_iterator =
        kh_put(k64_seq, barcode_lookup_table_, barcode_key, &khash_return_code);
    assert(khash_return_code != -1 && khash_return_code != 0);
    kh_value(barcode_lookup_table_, barcode_table_insert_iterator) =
        read_lookup_tables_.size();
    khash_t(k128) *read_lookup_table = kh_init(k128);
    khiter_t read_lookup_table_iterator =
        kh_put(k128, read_lookup_table, read_seed_key, &khash_return_code);
    assert(khash_return_code != -1 && khash_return_code != 0);
    uint128_t read_seeds = {.first = read1_seed2, .second = read2_seed2};
    kh_value(read_lookup_table, read_lookup_table_iterator) = read_seeds;
    read_lookup_tables_.push_back(read_lookup_table);
    // std::cerr << "No barcode, no read.\n";
    return false;
  }
}

uint32_t Chromap::SampleInputBarcodesAndExamineLength() {
  if (mapping_parameters_.is_bulk_data) {
    return 0;
  }

  uint32_t sample_batch_size = 1000;
  SequenceBatch barcode_batch(sample_batch_size, barcode_effective_range_);

  barcode_batch.InitializeLoading(mapping_parameters_.barcode_file_paths[0]);

  uint32_t num_loaded_barcodes = barcode_batch.LoadBatch();

  uint32_t cell_barcode_length = barcode_batch.GetSequenceLengthAt(0);
  for (uint32_t i = 1; i < num_loaded_barcodes; ++i) {
    if (barcode_batch.GetSequenceLengthAt(i) != cell_barcode_length) {
      ExitWithMessage("ERROR: barcode lengths are not equal in the sample!");
    }
  }

  barcode_batch.FinalizeLoading();

  return cell_barcode_length;
}

void Chromap::LoadBarcodeWhitelist() {
  double real_start_time = GetRealTime();
  int num_barcodes = 0;

  if (1) {
    gzFile barcode_whitelist_file = 
      gzopen(mapping_parameters_.barcode_whitelist_file_path.c_str(), "r"); 
    const uint32_t barcode_buffer_size = 256;
    char barcode[barcode_buffer_size];
    while (gzgets(barcode_whitelist_file, barcode, barcode_buffer_size) != NULL) {
      size_t barcode_length = strlen(barcode);
      if (barcode[barcode_length - 1] == '\n') {
        barcode[barcode_length - 1] = '\0';
        --barcode_length;
      }
      if (barcode_length > 32) {
        ExitWithMessage("ERROR: barcode length is greater than 32!");
      }

      if (barcode_length != barcode_length_) {
        if (num_barcodes == 0) {
          ExitWithMessage(
              "ERROR: whitelist and input barcode lengths are not equal!");
        } else {
          ExitWithMessage(
              "ERROR: barcode lengths are not equal in the whitelist!");
        }
      }
      
      uint64_t barcode_key = GenerateSeedFromSequence(
          barcode, barcode_length, 0, barcode_length);
      
      int khash_return_code;
      khiter_t barcode_whitelist_lookup_table_iterator =
        kh_put(k64_seq, barcode_whitelist_lookup_table_, barcode_key,
            &khash_return_code);
      kh_value(barcode_whitelist_lookup_table_,
          barcode_whitelist_lookup_table_iterator) = 0;
      assert(khash_return_code != -1 && khash_return_code != 0);
      ++num_barcodes;
    }
    if (!gzeof(barcode_whitelist_file)) {
      ExitWithMessage("ERROR: barcode whitelist file does not exist or is truncated!");
    }
    gzclose(barcode_whitelist_file);
  } else { 
    std::ifstream barcode_whitelist_file_stream(
        mapping_parameters_.barcode_whitelist_file_path);
    std::string barcode_whitelist_file_line;
    // bool first_line = true;
    while (getline(barcode_whitelist_file_stream, barcode_whitelist_file_line)) {
      std::stringstream barcode_whitelist_file_line_string_stream(
          barcode_whitelist_file_line);
      //// skip the header
      // if (barcode_whitelist_file_line[0] == '#' ||
      // barcode_whitelist_file_line.find("kmer") == 0) {
      //  continue;
      //}
      std::string barcode;
      barcode_whitelist_file_line_string_stream >> barcode;
      size_t barcode_length = barcode.length();
      if (barcode_length > 32) {
        ExitWithMessage("ERROR: barcode length is greater than 32!");
      }

      if (barcode_length != barcode_length_) {
        if (num_barcodes == 0) {
          ExitWithMessage(
              "ERROR: whitelist and input barcode lengths are not equal!");
        } else {
          ExitWithMessage(
              "ERROR: barcode lengths are not equal in the whitelist!");
        }
      }

      // if (first_line) {
      //  //size_t barcode_length = kmer.length();
      //  // Allocate memory to save pore model parameters
      //  //size_t num_pore_models = 1 << (kmer_size_ * 2);
      //  //pore_models_.assign(num_pore_models, PoreModelParameters());
      //  //first_line = false;
      //}
      // assert(kmer.length() == (size_t)kmer_size_);
      uint64_t barcode_key = GenerateSeedFromSequence(
          barcode.data(), barcode_length, 0, barcode_length);
      // PoreModelParameters &pore_model_parameters =
      // pore_models_[kmer_hash_value]; barcode_whitelist_file_line_string_stream
      // >> pore_model_parameters.level_mean >> pore_model_parameters.level_stdv
      // >> pore_model_parameters.sd_mean >> pore_model_parameters.sd_stdv;
      int khash_return_code;
      khiter_t barcode_whitelist_lookup_table_iterator =
        kh_put(k64_seq, barcode_whitelist_lookup_table_, barcode_key,
            &khash_return_code);
      kh_value(barcode_whitelist_lookup_table_,
          barcode_whitelist_lookup_table_iterator) = 0;
      assert(khash_return_code != -1 && khash_return_code != 0);
      ++num_barcodes;
    }
    barcode_whitelist_file_stream.close();
  }
  std::cerr << "Loaded " << num_barcodes << " barcodes in "
            << GetRealTime() - real_start_time << "s.\n";
}

void Chromap::ComputeBarcodeAbundance(uint64_t max_num_sample_barcodes) {
  double real_start_time = GetRealTime();
  SequenceBatch barcode_batch(read_batch_size_, barcode_effective_range_);
  for (size_t read_file_index = 0;
       read_file_index < mapping_parameters_.read_file1_paths.size();
       ++read_file_index) {
    barcode_batch.InitializeLoading(
        mapping_parameters_.barcode_file_paths[read_file_index]);
    uint32_t num_loaded_barcodes = barcode_batch.LoadBatch();
    while (num_loaded_barcodes > 0) {
      for (uint32_t barcode_index = 0; barcode_index < num_loaded_barcodes;
           ++barcode_index) {
        std::vector<int> N_pos;  // position of Ns
        barcode_batch.GetSequenceNsAt(barcode_index, /*little_endian=*/true,
                                      N_pos);
        if (N_pos.size() > 0) continue;

        uint32_t barcode_length =
            barcode_batch.GetSequenceLengthAt(barcode_index);
        uint64_t barcode_key = barcode_batch.GenerateSeedFromSequenceAt(
            barcode_index, 0, barcode_length);
        khiter_t barcode_whitelist_lookup_table_iterator =
            kh_get(k64_seq, barcode_whitelist_lookup_table_, barcode_key);
        if (barcode_whitelist_lookup_table_iterator !=
            kh_end(barcode_whitelist_lookup_table_)) {
          // Correct barcode
          kh_value(barcode_whitelist_lookup_table_,
                   barcode_whitelist_lookup_table_iterator) += 1;
          ++num_sample_barcodes_;
        }
      }
      if (!mapping_parameters_.skip_barcode_check &&
          num_sample_barcodes_ * 20 < num_loaded_barcodes) {
        // Since num_loaded_pairs is a constant, this if is actuaclly only
        // effective in the first iteration
        ExitWithMessage(
            "Less than 5\% barcodes can be found or corrected based on the "
            "barcode whitelist.\nPlease check whether the barcode whitelist "
            "matches the data, e.g. length, reverse-complement. If this is a "
            "false warning, please run Chromap with the option "
            "--skip-barcode-check.");
      }

      if (num_sample_barcodes_ >= max_num_sample_barcodes) {
        break;
      }
      num_loaded_barcodes = barcode_batch.LoadBatch();
    }
    barcode_batch.FinalizeLoading();
    if (num_sample_barcodes_ >= max_num_sample_barcodes) {
      break;
    }
  }

  std::cerr << "Compute barcode abundance using " << num_sample_barcodes_
            << " in " << GetRealTime() - real_start_time << "s.\n";
}

void Chromap::UpdateBarcodeAbundance(uint32_t num_loaded_barcodes,
                                     const SequenceBatch &barcode_batch) {
  double real_start_time = GetRealTime();
  for (uint32_t barcode_index = 0; barcode_index < num_loaded_barcodes;
       ++barcode_index) {
    uint32_t barcode_length = barcode_batch.GetSequenceLengthAt(barcode_index);
    uint64_t barcode_key = barcode_batch.GenerateSeedFromSequenceAt(
        barcode_index, 0, barcode_length);
    khiter_t barcode_whitelist_lookup_table_iterator =
        kh_get(k64_seq, barcode_whitelist_lookup_table_, barcode_key);
    if (barcode_whitelist_lookup_table_iterator !=
        kh_end(barcode_whitelist_lookup_table_)) {
      // Correct barcode
      kh_value(barcode_whitelist_lookup_table_,
               barcode_whitelist_lookup_table_iterator) += 1;
      ++num_sample_barcodes_;
    }
  }
  std::cerr << "Update barcode abundance using " << num_sample_barcodes_
            << " in " << GetRealTime() - real_start_time << "s.\n";
}

bool Chromap::CorrectBarcodeAt(uint32_t barcode_index,
                               SequenceBatch &barcode_batch,
                               uint64_t &num_barcode_in_whitelist,
                               uint64_t &num_corrected_barcode) {
  const uint32_t barcode_length =
      barcode_batch.GetSequenceLengthAt(barcode_index);
  const uint64_t barcode_key = barcode_batch.GenerateSeedFromSequenceAt(
      barcode_index, 0, barcode_length);
  khiter_t barcode_whitelist_lookup_table_iterator =
      kh_get(k64_seq, barcode_whitelist_lookup_table_, barcode_key);
  std::vector<int> N_pos;  // position of Ns

  barcode_batch.GetSequenceNsAt(barcode_index, /*little_endian=*/true, N_pos);
  if (N_pos.size() >
      (uint32_t)mapping_parameters_.barcode_correction_error_threshold)
    return false;

  if (N_pos.size() == 0 && barcode_whitelist_lookup_table_iterator !=
                               kh_end(barcode_whitelist_lookup_table_)) {
    // Correct barcode
    ++num_barcode_in_whitelist;
    return true;
  } else if (mapping_parameters_.barcode_correction_error_threshold > 0) {
    // Need to correct this barcode
    // const char *barcode = barcode_batch->GetSequenceAt(barcode_index);
    // std::cerr << barcode_index << " barcode " << barcode << " needs
    // correction\n";
    const char *barcode_qual = barcode_batch.GetSequenceQualAt(barcode_index);
    std::vector<BarcodeWithQual> corrected_barcodes_with_quals;
    uint64_t mask = (uint64_t)3;
    uint32_t i_start = 0;
    uint32_t i_end = barcode_length;
    uint32_t ti_limit = 3;
    if (N_pos.size() > 0) {
      i_start = N_pos[0];
      i_end = N_pos[0] + 1;
      ti_limit = 4;
    }
    for (uint32_t i = i_start; i < i_end; ++i) {
      uint64_t barcode_key_to_change = mask << (2 * i);
      barcode_key_to_change = ~barcode_key_to_change;
      barcode_key_to_change &= barcode_key;
      uint64_t base_to_change1 = (barcode_key >> (2 * i)) & mask;
      for (uint32_t ti = 0; ti < ti_limit; ++ti) {
        // change the base
        base_to_change1 += 1;
        base_to_change1 &= mask;
        // generate the corrected key
        uint64_t corrected_barcode_key =
            barcode_key_to_change | (base_to_change1 << (2 * i));
        barcode_whitelist_lookup_table_iterator = kh_get(
            k64_seq, barcode_whitelist_lookup_table_, corrected_barcode_key);
        if (barcode_whitelist_lookup_table_iterator !=
            kh_end(barcode_whitelist_lookup_table_)) {
          // find one possible corrected barcode
          double barcode_abundance =
              kh_value(barcode_whitelist_lookup_table_,
                       barcode_whitelist_lookup_table_iterator) /
              (double)num_sample_barcodes_;
          int qual_offset = 33;
          int adjusted_qual =
              barcode_qual[barcode_length - 1 - i] - qual_offset;
          adjusted_qual = adjusted_qual > 40 ? 40 : adjusted_qual;
          adjusted_qual = adjusted_qual < 3 ? 3 : adjusted_qual;
          double score =
              pow(10.0, ((-adjusted_qual) / 10.0)) * barcode_abundance;
          corrected_barcodes_with_quals.emplace_back(
              BarcodeWithQual{barcode_length - 1 - i,
                              Uint8ToChar(base_to_change1), 0, 0, score});
          // std::cerr << "1score: " << score << " pos1: " << barcode_length - 1
          // - i << " b1: " << base_to_change1 << " pos2: " << 0 << " b2: " <<
          // (char)0 << "\n";
        }
        if (mapping_parameters_.barcode_correction_error_threshold == 2) {
          uint32_t j_start = i + 1;
          uint32_t j_end = barcode_length;
          uint32_t ti2_limit = 3;
          if (N_pos.size() == 2) {
            j_start = N_pos[1];
            j_end = N_pos[1] + 1;
            ti2_limit = 4;
          }
          for (uint32_t j = j_start; j < j_end; ++j) {
            uint64_t barcode_key_to_change2 = mask << (2 * i);
            barcode_key_to_change2 = mask << (2 * j);
            barcode_key_to_change2 = ~barcode_key_to_change2;
            barcode_key_to_change2 &= corrected_barcode_key;
            uint64_t base_to_change2 =
                (corrected_barcode_key >> (2 * j)) & mask;
            for (uint32_t ti2 = 0; ti2 < ti2_limit; ++ti2) {
              // change the base
              base_to_change2 += 1;
              base_to_change2 &= mask;
              // generate the corrected key
              uint64_t corrected_barcode_key2 =
                  barcode_key_to_change2 | (base_to_change2 << (2 * j));
              barcode_whitelist_lookup_table_iterator =
                  kh_get(k64_seq, barcode_whitelist_lookup_table_,
                         corrected_barcode_key2);
              if (barcode_whitelist_lookup_table_iterator !=
                  kh_end(barcode_whitelist_lookup_table_)) {
                // find one possible corrected barcode
                double barcode_abundance =
                    kh_value(barcode_whitelist_lookup_table_,
                             barcode_whitelist_lookup_table_iterator) /
                    (double)num_sample_barcodes_;
                int qual_offset = 33;
                int adjusted_qual =
                    barcode_qual[barcode_length - 1 - j] - qual_offset;
                adjusted_qual = adjusted_qual > 40 ? 40 : adjusted_qual;
                adjusted_qual = adjusted_qual < 3 ? 3 : adjusted_qual;
                int adjusted_qual1 =
                    barcode_qual[barcode_length - 1 - i] - qual_offset;
                adjusted_qual1 = adjusted_qual1 > 40 ? 40 : adjusted_qual1;
                adjusted_qual1 = adjusted_qual1 < 3 ? 3 : adjusted_qual1;
                adjusted_qual += adjusted_qual1;
                double score =
                    pow(10.0, ((-adjusted_qual) / 10.0)) * barcode_abundance;
                corrected_barcodes_with_quals.emplace_back(BarcodeWithQual{
                    barcode_length - 1 - i, Uint8ToChar(base_to_change1),
                    barcode_length - 1 - j, Uint8ToChar(base_to_change2),
                    score});
                // std::cerr << "2score: " << score << " pos1: " <<
                // barcode_length - 1 - i << " b1: " << base_to_change1 << "
                // pos2: " << barcode_length - 1 -j << " b2: " <<
                // base_to_change2
                // << "\n";
              }
            }
          }
        }
      }
    }
    size_t num_possible_corrected_barcodes =
        corrected_barcodes_with_quals.size();
    if (num_possible_corrected_barcodes == 0) {
      // Barcode cannot be corrected, leave it for downstream
      return false;
    } else if (num_possible_corrected_barcodes == 1) {
      // Just correct it
      // std::cerr << "Corrected the barcode from " << barcode << " to ";
      barcode_batch.CorrectBaseAt(
          barcode_index, corrected_barcodes_with_quals[0].corrected_base_index1,
          corrected_barcodes_with_quals[0].correct_base1);
      if (corrected_barcodes_with_quals[0].correct_base2 != 0) {
        barcode_batch.CorrectBaseAt(
            barcode_index,
            corrected_barcodes_with_quals[0].corrected_base_index2,
            corrected_barcodes_with_quals[0].correct_base2);
      }
      // std::cerr << barcode << "\n";
      // std::cerr << "score: " << corrected_barcodes_with_quals[0].score <<
      // "\n"; std::cerr << "score: " << corrected_barcodes_with_quals[0].score
      // << " pos1: " << corrected_barcodes_with_quals[0].corrected_base_index1
      // << " b1: " << corrected_barcodes_with_quals[0].correct_base1 << " pos2:
      // " << corrected_barcodes_with_quals[0].corrected_base_index2 << " b2: "
      // << corrected_barcodes_with_quals[0].correct_base2 << "\n";
      ++num_corrected_barcode;
      return true;
    } else {
      // Select the best correction
      std::sort(corrected_barcodes_with_quals.begin(),
                corrected_barcodes_with_quals.end(),
                std::greater<BarcodeWithQual>());
      // int num_ties = 0;
      double sum_score = 0;
      for (size_t ci = 0; ci < num_possible_corrected_barcodes; ++ci) {
        sum_score += corrected_barcodes_with_quals[ci].score;
        // std::cerr << ci << " score: " <<
        // corrected_barcodes_with_quals[ci].score << " pos1: " <<
        // corrected_barcodes_with_quals[ci].corrected_base_index1 << " b1: " <<
        // corrected_barcodes_with_quals[ci].correct_base1 << " pos2: " <<
        // corrected_barcodes_with_quals[ci].corrected_base_index2 << " b2: " <<
        // corrected_barcodes_with_quals[ci].correct_base2 << "\n"; if
        // (corrected_barcodes_with_quals[ci].qual ==
        // corrected_barcodes_with_quals[0].qual) {
        //  ++num_ties;
        //}
      }
      int best_corrected_barcode_index = 0;
      // if (num_ties > 0) {
      //  std::mt19937 tmp_generator(11);
      //  std::uniform_int_distribution<int> distribution(0, num_ties); //
      //  important: inclusive range best_corrected_barcode_index =
      //  distribution(tmp_generator);
      //}
      // std::cerr << "Corrected the barcode from " << barcode << " to ";
      double confidence_threshold =
          mapping_parameters_.barcode_correction_probability_threshold;
      if (corrected_barcodes_with_quals[best_corrected_barcode_index].score /
              sum_score >
          confidence_threshold) {
        barcode_batch.CorrectBaseAt(
            barcode_index,
            corrected_barcodes_with_quals[best_corrected_barcode_index]
                .corrected_base_index1,
            corrected_barcodes_with_quals[best_corrected_barcode_index]
                .correct_base1);
        if (corrected_barcodes_with_quals[best_corrected_barcode_index]
                .correct_base2 != 0) {
          barcode_batch.CorrectBaseAt(
              barcode_index,
              corrected_barcodes_with_quals[best_corrected_barcode_index]
                  .corrected_base_index2,
              corrected_barcodes_with_quals[best_corrected_barcode_index]
                  .correct_base2);
        }
        // std::cerr << barcode << "\n";
        // std::cerr << "score: " <<
        // corrected_barcodes_with_quals[best_corrected_barcode_index].score <<
        // "\n"; std::cerr << "best score: " <<
        // corrected_barcodes_with_quals[best_corrected_barcode_index].score <<
        // " sum score: " << sum_score << "\n";
        ++num_corrected_barcode;
        return true;
      } else {
        // std::cerr << "Didnt pass filter: " <<
        // corrected_barcodes_with_quals[best_corrected_barcode_index].score /
        // sum_score << "\n"; std::cerr << "best score: " <<
        // corrected_barcodes_with_quals[best_corrected_barcode_index].score <<
        // " sum score: " << sum_score << "\n";
        return false;
      }
    }
  } else {
    return false;
  }
}

void Chromap::OutputBarcodeStatistics() {
  std::cerr << "Number of barcodes in whitelist: " << num_barcode_in_whitelist_
            << ".\n";
  std::cerr << "Number of corrected barcodes: " << num_corrected_barcode_
            << ".\n";
}

void Chromap::OutputMappingStatistics() {
  std::cerr << "Number of reads: " << num_reads_ << ".\n";
  // std::cerr << "Number of duplicated reads: " << num_duplicated_reads_ <<
  // ".\n";
  std::cerr << "Number of mapped reads: " << num_mapped_reads_ << ".\n";
  std::cerr << "Number of uniquely mapped reads: " << num_uniquely_mapped_reads_
            << ".\n";
  std::cerr << "Number of reads have multi-mappings: "
            << num_mapped_reads_ - num_uniquely_mapped_reads_ << ".\n";
  std::cerr << "Number of candidates: " << num_candidates_ << ".\n";
  std::cerr << "Number of mappings: " << num_mappings_ << ".\n";
  std::cerr << "Number of uni-mappings: " << num_uniquely_mapped_reads_
            << ".\n";
  std::cerr << "Number of multi-mappings: "
            << num_mappings_ - num_uniquely_mapped_reads_ << ".\n";
}

void Chromap::ParseReadFormat(const std::string &read_format) {
  if (read_format.empty()) {
    return;
  }

  read1_effective_range_.InitializeParsing();
  read2_effective_range_.InitializeParsing();
  barcode_effective_range_.InitializeParsing();

  uint32_t i, j;
  for (i = 0; i < read_format.size();) {
    for (j = i + 1; j < read_format.size() && read_format[j] != ','; ++j)
      ;
    bool parse_success = true;
    if (read_format[i] == 'r' && read_format[i + 1] == '1') {
      parse_success =
          read1_effective_range_.ParseFormatStringAndAppendEffectiveRange(
              read_format.c_str() + i, j - i);
    } else if (read_format[i] == 'r' && read_format[i + 1] == '2') {
      parse_success =
          read2_effective_range_.ParseFormatStringAndAppendEffectiveRange(
              read_format.c_str() + i, j - i);
    } else if (read_format[i] == 'b' && read_format[i + 1] == 'c') {
      parse_success =
          barcode_effective_range_.ParseFormatStringAndAppendEffectiveRange(
              read_format.c_str() + i, j - i);
    } else {
      parse_success = false;
    }

    if (!parse_success) {
      ExitWithMessage("Unknown read format: " + read_format + "\n");
    }

    i = j + 1;
  }

  read1_effective_range_.FinalizeParsing();
  read2_effective_range_.FinalizeParsing();
  barcode_effective_range_.FinalizeParsing();
}

void Chromap::GenerateCustomRidRanks(
    const std::string &custom_rid_order_file_path,
    uint32_t num_reference_sequences, const SequenceBatch &reference,
    std::vector<int> &rid_ranks) {
  for (uint32_t i = 0; i < num_reference_sequences; ++i) {
    rid_ranks.emplace_back(i);
  }

  if (custom_rid_order_file_path.empty()) {
    return;
  }

  std::unordered_map<std::string, int> ref_name_to_rank;
  std::ifstream custom_rid_order_file_stream(custom_rid_order_file_path);
  std::string ref_name;
  uint32_t ref_rank = 0;
  while (getline(custom_rid_order_file_stream, ref_name)) {
    ref_name_to_rank[ref_name] = ref_rank;
    ref_rank += 1;
  }
  custom_rid_order_file_stream.close();

  // First, rank the chromosomes in the custom order provided by users.
  for (uint32_t i = 0; i < num_reference_sequences; ++i) {
    std::string ref_name(reference.GetSequenceNameAt(i));
    if (ref_name_to_rank.find(ref_name) != ref_name_to_rank.end()) {
      rid_ranks[i] = ref_name_to_rank[ref_name];
    } else {
      rid_ranks[i] = -1;
    }
  }

  // There might be some rids without any custom order. We just order them based
  // on their original order in the reference file.
  uint32_t k = ref_name_to_rank.size();
  // Rank the remaining chromosomes.
  for (uint32_t i = 0; i < num_reference_sequences; ++i) {
    if (rid_ranks[i] == -1) {
      rid_ranks[i] = k;
      ++k;
    }
  }

  if (k > num_reference_sequences) {
    ExitWithMessage(
        "ERROR: unknown chromsome names found in chromosome order file.");
  }
}

void Chromap::RerankCandidatesRid(std::vector<Candidate> &candidates) {
  for (size_t i = 0; i < candidates.size(); ++i) {
    uint64_t rid = (uint32_t)(candidates[i].position >> 32);
    rid = custom_rid_rank_[rid];
    candidates[i].position =
        (candidates[i].position & (uint64_t)0xffffffff) | (rid << 32);
  }
}

}  // namespace chromap
