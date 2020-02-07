#include "chromap.h"

#include <assert.h>
#include <iostream> 
#include <limits>
#include <math.h>
#include <omp.h>
#include <random>

#include "cxxopts.hpp"
#include "ksw.h"

namespace chromap {
template <typename MappingRecord>
void Chromap<MappingRecord>::TrimAdapterForPairedEndRead(uint32_t pair_index, SequenceBatch *read_batch1, SequenceBatch *read_batch2) {
  const char *read1 = read_batch1->GetSequenceAt(pair_index);
  uint32_t read2_length = read_batch2->GetSequenceLengthAt(pair_index);
  const std::string &negative_read2 = read_batch2->GetNegativeSequenceAt(pair_index);
  int min_overlap_length = min_read_length_;
  int seed_length = min_overlap_length / 2;
  int error_threshold_for_merging = 1;
  bool is_merged = false;
  for (int si = 0; si < error_threshold_for_merging + 1; ++si) {
    int seed_start_position = negative_read2.find(read1 + si * seed_length, 0, seed_length);
    while ((uint32_t)seed_start_position != std::string::npos && read2_length - seed_start_position + seed_length * si >= (uint32_t)min_overlap_length && seed_start_position >= si * seed_length) {
      bool can_merge = true;
      int num_errors = 0;
      for (int i = 0; i < seed_length * si; ++i) {
        if (negative_read2[seed_start_position - si * seed_length + i] != read1[i]) {
          ++num_errors;
        }
        if (num_errors > error_threshold_for_merging) {
          can_merge = false;
          break;
        }
      }
      for (uint32_t i = seed_length; i + seed_start_position < read2_length; ++i) {
        if (negative_read2[seed_start_position + i] != read1[si * seed_length + i]) {
          ++num_errors;
        }
        if (num_errors > error_threshold_for_merging) {
          can_merge = false;
          break;
        }
      }
      if (can_merge) {
        // Trim adapters and TODO: fix sequencing errors
        int overlap_length = read2_length - seed_start_position + si * seed_length;
        read_batch1->TrimSequenceAt(pair_index, overlap_length);
        read_batch2->TrimSequenceAt(pair_index, overlap_length);
        is_merged = true;
        //std::cerr << "Trimed! overlap length: " << overlap_length << ", " << read1.GetLength() << " " << read2.GetLength() << "\n";
        break;
      }
      seed_start_position = negative_read2.find(read1 + si * seed_length, seed_start_position + 1, seed_length);
    }
    if (is_merged) {
      break;
    }
  }
}

template <typename MappingRecord>
bool Chromap<MappingRecord>::PairedEndReadWithBarcodeIsDuplicate(uint32_t pair_index, const SequenceBatch &barcode_batch, const SequenceBatch &read_batch1, const SequenceBatch &read_batch2) {
  int dedupe_seed_length = 16;
  uint32_t barcode_key = barcode_batch.GenerateSeedFromSequenceAt(pair_index, 0, dedupe_seed_length);
  uint64_t read1_seed1 = read_batch1.GenerateSeedFromSequenceAt(pair_index, 0, dedupe_seed_length);
  uint64_t read2_seed1 = read_batch2.GenerateSeedFromSequenceAt(pair_index, 0, dedupe_seed_length);
  uint64_t read_seed_key = (read1_seed1 << (dedupe_seed_length * 2)) | read2_seed1;
  uint64_t read1_seed2 = read_batch1.GenerateSeedFromSequenceAt(pair_index, dedupe_seed_length, dedupe_seed_length * 2);
  uint64_t read2_seed2 = read_batch2.GenerateSeedFromSequenceAt(pair_index, dedupe_seed_length, dedupe_seed_length * 2);
  khiter_t barcode_table_iterator = kh_get(k32, barcode_lookup_table_, barcode_key);
  if (barcode_table_iterator != kh_end(barcode_lookup_table_)) {
    uint32_t read_lookup_table_index = kh_value(barcode_lookup_table_, barcode_table_iterator);
    //std::cerr << "Have barcode, try to check read. " << read_lookup_table_index << "\n";
    khash_t(k128) *read_lookup_table = read_lookup_tables_[read_lookup_table_index];
    khiter_t read_lookup_table_iterator = kh_get(k128, read_lookup_table, read_seed_key);
    if (read_lookup_table_iterator != kh_end(read_lookup_table)) {
      //std::cerr << "Have barcode, have read, try whether match.\n";
      uint128_t read_seeds = kh_value(read_lookup_table, read_lookup_table_iterator);
      if (read_seeds.first == read1_seed2 && read_seeds.second == read2_seed2) {
        //std::cerr << "Have barcode, have read, and match.\n";
        return true;
      } else {
        //std::cerr << "Have barcode, have read, but don't match.\n";
        return false;
      }
    } else {
      //std::cerr << "Have barcode, no read.\n";
      uint128_t read_seeds = {.first = read1_seed2, .second = read2_seed2};
      int khash_return_code;
      khiter_t read_lookup_table_insert_iterator = kh_put(k128, read_lookup_table, read_seed_key, &khash_return_code);
      assert(khash_return_code != -1 && khash_return_code != 0);
      kh_value(read_lookup_table, read_lookup_table_insert_iterator) = read_seeds;
      //std::cerr << "Have barcode, no read.\n";
      return false;
    }
  } else {
    // insert the barcode and append a new read hash table to tables and then insert the reads
    //std::cerr << "No barcode, no read.\n";
    int khash_return_code;
    khiter_t barcode_table_insert_iterator = kh_put(k32, barcode_lookup_table_, barcode_key, &khash_return_code);
    assert(khash_return_code != -1 && khash_return_code != 0);
    kh_value(barcode_lookup_table_, barcode_table_insert_iterator) = read_lookup_tables_.size();
    khash_t(k128) *read_lookup_table = kh_init(k128);
    khiter_t read_lookup_table_iterator = kh_put(k128, read_lookup_table, read_seed_key, &khash_return_code);
    assert(khash_return_code != -1 && khash_return_code != 0);
    uint128_t read_seeds = {.first = read1_seed2, .second = read2_seed2};
    kh_value(read_lookup_table, read_lookup_table_iterator) = read_seeds;
    read_lookup_tables_.push_back(read_lookup_table);
    if (kh_size(barcode_lookup_table_) >= (uint32_t)allocated_barcode_lookup_table_size_) {
      allocated_barcode_lookup_table_size_ <<= 1;
      kh_resize(k32, barcode_lookup_table_, allocated_barcode_lookup_table_size_);
    }
    //std::cerr << "No barcode, no read.\n";
    return false;
  }
}

template <typename MappingRecord>
uint32_t Chromap<MappingRecord>::LoadPairedEndReadsWithBarcodes(SequenceBatch *read_batch1, SequenceBatch *read_batch2, SequenceBatch *barcode_batch) {
  double real_start_time = Chromap<>::GetRealTime();
  uint32_t num_loaded_pairs = 0;
  while (num_loaded_pairs < read_batch_size_) {
    bool no_more_read1 = read_batch1->LoadOneSequenceAndSaveAt(num_loaded_pairs);
    bool no_more_read2 = read_batch2->LoadOneSequenceAndSaveAt(num_loaded_pairs);
    bool no_more_barcode = no_more_read2;
    if (!is_bulk_data_) {
      no_more_barcode = barcode_batch->LoadOneSequenceAndSaveAt(num_loaded_pairs);
    }
    if ((!no_more_read1) && (!no_more_read2) && (!no_more_barcode)) {
      if (read_batch1->GetSequenceLengthAt(num_loaded_pairs) < (uint32_t)min_read_length_ || read_batch2->GetSequenceLengthAt(num_loaded_pairs) < (uint32_t)min_read_length_) {
        continue; // reads are too short, just drop.
      }
      //if (PairedEndReadWithBarcodeIsDuplicate(num_loaded_pairs, (*barcode_batch), (*read_batch1), (*read_batch2))) {
      //  num_duplicated_reads_ += 2;
      //  continue;
      //}
    } else if (no_more_read1 && no_more_read2 && no_more_barcode) {
      break;
    } else {
      Chromap<>::ExitWithMessage("Numbers of reads and barcodes don't match!");
    }
    ++num_loaded_pairs;
  }
  if (num_loaded_pairs > 0) {
    std::cerr << "Loaded " << num_loaded_pairs << " pairs in "<< Chromap<>::GetRealTime() - real_start_time << "s.\n";
  } else {
    std::cerr << "No more reads.\n";
  }
  return num_loaded_pairs;
}

template <typename MappingRecord>
void Chromap<MappingRecord>::MapPairedEndReads() {
  // TODO(Haowen): check args for paired-end read mapping
  double real_start_time = Chromap<>::GetRealTime();
  SequenceBatch reference;
  reference.InitializeLoading(reference_file_path_);
  uint32_t num_reference_sequences = reference.LoadAllSequences();
  Index index(min_num_seeds_required_for_mapping_, max_seed_frequencies_, index_file_path_);
  index.Load();
  //index.Statistics(num_sequences, reference);
  SequenceBatch read_batch1(read_batch_size_);
  SequenceBatch read_batch2(read_batch_size_);
  SequenceBatch barcode_batch(read_batch_size_);
  SequenceBatch read_batch1_for_loading(read_batch_size_);
  SequenceBatch read_batch2_for_loading(read_batch_size_);
  SequenceBatch barcode_batch_for_loading(read_batch_size_);
  read_batch1_for_loading.InitializeLoading(read_file1_path_);
  read_batch2_for_loading.InitializeLoading(read_file2_path_);
  if (!is_bulk_data_) {
    barcode_batch_for_loading.InitializeLoading(barcode_file_path_);
  }
  double real_start_mapping_time = Chromap<>::GetRealTime();
  uint32_t num_loaded_pairs_for_loading = 0;
  uint32_t num_loaded_pairs = LoadPairedEndReadsWithBarcodes(&read_batch1_for_loading, &read_batch2_for_loading, &barcode_batch_for_loading);
  read_batch1_for_loading.SwapSequenceBatch(read_batch1);
  read_batch2_for_loading.SwapSequenceBatch(read_batch2);
  barcode_batch_for_loading.SwapSequenceBatch(barcode_batch);
  mappings_on_diff_ref_seqs_.reserve(num_reference_sequences);
  deduped_mappings_on_diff_ref_seqs_.reserve(num_reference_sequences);
  for (uint32_t i = 0; i < num_reference_sequences; ++i) {
    mappings_on_diff_ref_seqs_.emplace_back(std::vector<MappingRecord>());
    deduped_mappings_on_diff_ref_seqs_.emplace_back(std::vector<MappingRecord>());
  }
  std::vector<std::vector<std::vector<MappingRecord> > > mappings_on_diff_ref_seqs_for_diff_threads;
  std::vector<std::vector<std::vector<MappingRecord> > > mappings_on_diff_ref_seqs_for_diff_threads_for_saving;
  mappings_on_diff_ref_seqs_for_diff_threads.reserve(num_threads_);
  mappings_on_diff_ref_seqs_for_diff_threads_for_saving.reserve(num_threads_);
  for (int ti = 0; ti < num_threads_; ++ti) {
    mappings_on_diff_ref_seqs_for_diff_threads.emplace_back(std::vector<std::vector<MappingRecord> >(num_reference_sequences));
    mappings_on_diff_ref_seqs_for_diff_threads_for_saving.emplace_back(std::vector<std::vector<MappingRecord> >(num_reference_sequences));
    for (uint32_t i = 0; i < num_reference_sequences; ++i) {
      mappings_on_diff_ref_seqs_for_diff_threads[ti][i].reserve((num_loaded_pairs + num_loaded_pairs / 1000 * max_num_best_mappings_) / num_threads_ / num_reference_sequences);
      mappings_on_diff_ref_seqs_for_diff_threads_for_saving[ti][i].reserve((num_loaded_pairs + num_loaded_pairs / 1000 * max_num_best_mappings_) / num_threads_ / num_reference_sequences);
    }
  }
  if (output_mapping_in_BED_) {
    output_tools = std::unique_ptr<BEDPEOutputTools<MappingRecord> >(new BEDPEOutputTools<MappingRecord>);
  } else if (output_mapping_in_TagAlign_) {
    output_tools = std::unique_ptr<PairedTagAlignOutputTools<MappingRecord> >(new PairedTagAlignOutputTools<MappingRecord>);
  } else if (output_mapping_in_PAF_) {
    output_tools = std::unique_ptr<PairedPAFOutputTools<MappingRecord> >(new PairedPAFOutputTools<MappingRecord>);
  }
  output_tools->InitializeMappingOutput(mapping_output_file_path_);
  static uint64_t thread_num_candidates = 0;
  static uint64_t thread_num_mappings = 0;
  static uint64_t thread_num_mapped_reads = 0; 
  static uint64_t thread_num_uniquely_mapped_reads = 0; 
#pragma omp threadprivate(thread_num_candidates, thread_num_mappings, thread_num_mapped_reads, thread_num_uniquely_mapped_reads)
#pragma omp parallel default(none) shared(reference, index, read_batch1, read_batch2, barcode_batch, read_batch1_for_loading, read_batch2_for_loading, barcode_batch_for_loading, std::cerr, num_loaded_pairs_for_loading, num_loaded_pairs, num_reference_sequences, mappings_on_diff_ref_seqs_for_diff_threads, mappings_on_diff_ref_seqs_for_diff_threads_for_saving) num_threads(num_threads_) reduction(+:num_candidates_, num_mappings_, num_mapped_reads_, num_uniquely_mapped_reads_)
  {
  std::vector<std::pair<uint64_t, uint64_t> > minimizers1;
  std::vector<std::pair<uint64_t, uint64_t> > minimizers2;
  std::vector<uint64_t> positive_hits1;
  std::vector<uint64_t> positive_hits2;
  std::vector<uint64_t> negative_hits1;
  std::vector<uint64_t> negative_hits2;
  positive_hits1.reserve(max_seed_frequencies_[0]);
  positive_hits2.reserve(max_seed_frequencies_[0]);
  negative_hits1.reserve(max_seed_frequencies_[0]);
  negative_hits2.reserve(max_seed_frequencies_[0]);
  std::vector<uint64_t> positive_candidates1;
  std::vector<uint64_t> positive_candidates2;
  std::vector<uint64_t> negative_candidates1;
  std::vector<uint64_t> negative_candidates2;
  positive_candidates1.reserve(max_seed_frequencies_[0]);
  positive_candidates2.reserve(max_seed_frequencies_[0]);
  negative_candidates1.reserve(max_seed_frequencies_[0]);
  negative_candidates2.reserve(max_seed_frequencies_[0]);
  std::vector<std::pair<int, uint64_t> > positive_mappings1;
  std::vector<std::pair<int, uint64_t> > positive_mappings2;
  std::vector<std::pair<int, uint64_t> > negative_mappings1;
  std::vector<std::pair<int, uint64_t> > negative_mappings2;
  positive_mappings1.reserve(max_seed_frequencies_[0]);
  positive_mappings2.reserve(max_seed_frequencies_[0]);
  negative_mappings1.reserve(max_seed_frequencies_[0]);
  negative_mappings2.reserve(max_seed_frequencies_[0]);
  std::vector<std::pair<uint32_t, uint32_t> > F1R2_best_mappings;
  std::vector<std::pair<uint32_t, uint32_t> > F2R1_best_mappings;
  F1R2_best_mappings.reserve(max_seed_frequencies_[0]);
  F2R1_best_mappings.reserve(max_seed_frequencies_[0]);
  // we will use reservoir sampling 
  std::vector<int> best_mapping_indices(max_num_best_mappings_);
  std::mt19937 generator(11);
#pragma omp single
  {
  while (num_loaded_pairs > 0) {
    double real_batch_start_time = Chromap<>::GetRealTime();
    num_reads_ += num_loaded_pairs;
    num_reads_ += num_loaded_pairs;
#pragma omp task
    {
    num_loaded_pairs_for_loading = LoadPairedEndReadsWithBarcodes(&read_batch1_for_loading, &read_batch2_for_loading, &barcode_batch_for_loading);
    } // end of openmp loading task
    int grain_size = 10000;
#pragma omp taskloop grainsize(grain_size) //num_tasks(num_threads_* 50)
    for (uint32_t pair_index = 0; pair_index < num_loaded_pairs; ++pair_index) {
      read_batch1.PrepareNegativeSequenceAt(pair_index);
      read_batch2.PrepareNegativeSequenceAt(pair_index);
      if (trim_adapters_) {
        TrimAdapterForPairedEndRead(pair_index, &read_batch1, &read_batch2);
      }
      minimizers1.clear();
      minimizers2.clear();
      minimizers1.reserve(read_batch1.GetSequenceLengthAt(pair_index) / window_size_ * 2);
      minimizers2.reserve(read_batch2.GetSequenceLengthAt(pair_index) / window_size_ * 2);
      index.GenerateMinimizerSketch(read_batch1, pair_index, &minimizers1);
      index.GenerateMinimizerSketch(read_batch2, pair_index, &minimizers2);
      if (minimizers1.size() != 0 && minimizers2.size() != 0) {
        positive_hits1.clear();
        positive_hits2.clear();
        negative_hits1.clear();
        negative_hits2.clear();
        positive_candidates1.clear();
        positive_candidates2.clear();
        negative_candidates1.clear();
        negative_candidates2.clear();
        index.GenerateCandidates(minimizers1, &positive_hits1, &negative_hits1, &positive_candidates1, &negative_candidates1);
        uint32_t current_num_candidates1 = positive_candidates1.size() + negative_candidates1.size();
        index.GenerateCandidates(minimizers2, &positive_hits2, &negative_hits2, &positive_candidates2, &negative_candidates2);
        uint32_t current_num_candidates2 = positive_candidates2.size() + negative_candidates2.size();
        if (current_num_candidates1 > 0 && current_num_candidates2 > 0) {
          positive_candidates1.swap(positive_hits1);
          negative_candidates1.swap(negative_hits1);
          positive_candidates2.swap(positive_hits2);
          negative_candidates2.swap(negative_hits2);
          positive_candidates1.clear();
          positive_candidates2.clear();
          negative_candidates1.clear();
          negative_candidates2.clear();
          ReduceCandidatesForPairedEndRead(positive_hits1, negative_hits1, positive_hits2, negative_hits2, &positive_candidates1, &negative_candidates1, &positive_candidates2, &negative_candidates2);
          thread_num_candidates += positive_candidates1.size() + positive_candidates2.size() + negative_candidates1.size() + negative_candidates2.size();
          positive_mappings1.clear();
          positive_mappings2.clear();
          negative_mappings1.clear();
          negative_mappings2.clear();
          int min_num_errors1, second_min_num_errors1;
          int num_best_mappings1, num_second_best_mappings1;
          int min_num_errors2, second_min_num_errors2;
          int num_best_mappings2, num_second_best_mappings2;
          VerifyCandidates(read_batch1, pair_index, reference, positive_candidates1, negative_candidates1, &positive_mappings1, &negative_mappings1, &min_num_errors1, &num_best_mappings1, &second_min_num_errors1, &num_second_best_mappings1);
          uint32_t current_num_mappings1 = positive_mappings1.size() + negative_mappings1.size();
          VerifyCandidates(read_batch2, pair_index, reference, positive_candidates2, negative_candidates2, &positive_mappings2, &negative_mappings2, &min_num_errors2, &num_best_mappings2, &second_min_num_errors2, &num_second_best_mappings2);
          uint32_t current_num_mappings2 = positive_mappings2.size() + negative_mappings2.size();
          if (current_num_mappings1 > 0 && current_num_mappings2 > 0) {
            int min_sum_errors, second_min_sum_errors;
            int num_best_mappings, num_second_best_mappings;
            F1R2_best_mappings.clear();
            F2R1_best_mappings.clear();
            std::vector<std::vector<MappingRecord> > &mappings_on_diff_ref_seqs = mappings_on_diff_ref_seqs_for_diff_threads[omp_get_thread_num()];
            GenerateBestMappingsForPairedEndRead(pair_index, positive_candidates1.size(), negative_candidates1.size(), min_num_errors1, num_best_mappings1, second_min_num_errors1, num_second_best_mappings1, read_batch1, positive_mappings1, negative_mappings1, positive_candidates2.size(), negative_candidates2.size(), min_num_errors2, num_best_mappings2, second_min_num_errors2, num_second_best_mappings2, read_batch2, reference, barcode_batch, positive_mappings2, negative_mappings2, &best_mapping_indices, &generator, &F1R2_best_mappings, &F2R1_best_mappings, &min_sum_errors, &num_best_mappings, &second_min_sum_errors, &num_second_best_mappings, &mappings_on_diff_ref_seqs);
            if (num_best_mappings == 1) {
              ++thread_num_uniquely_mapped_reads;
              ++thread_num_uniquely_mapped_reads;
            }
            thread_num_mappings += std::min(num_best_mappings, max_num_best_mappings_);
            thread_num_mappings += std::min(num_best_mappings, max_num_best_mappings_);
            if (num_best_mappings > 0) {
              ++thread_num_mapped_reads;
              ++thread_num_mapped_reads;
            }
          }
        }
      }
    }
#pragma omp taskwait
    num_loaded_pairs = num_loaded_pairs_for_loading;
    read_batch1_for_loading.SwapSequenceBatch(read_batch1);
    read_batch2_for_loading.SwapSequenceBatch(read_batch2);
    barcode_batch_for_loading.SwapSequenceBatch(barcode_batch);
    mappings_on_diff_ref_seqs_for_diff_threads.swap(mappings_on_diff_ref_seqs_for_diff_threads_for_saving);
#pragma omp task
    {
    MoveMappingsInBuffersToMappingContainer(num_reference_sequences, &mappings_on_diff_ref_seqs_for_diff_threads_for_saving);
    }
    std::cerr << "Mapped in " << Chromap<>::GetRealTime() - real_batch_start_time << "s.\n";
  }
  } // end of openmp single
  num_candidates_ += thread_num_candidates;
  num_mappings_ += thread_num_mappings;
  num_mapped_reads_ += thread_num_mapped_reads;
  num_uniquely_mapped_reads_ += thread_num_uniquely_mapped_reads;
  } // end of openmp parallel region
  read_batch1_for_loading.FinalizeLoading();
  read_batch2_for_loading.FinalizeLoading();
  if (!is_bulk_data_) {
    barcode_batch_for_loading.FinalizeLoading();
  }
  std::cerr << "Mapped all reads in " << Chromap<>::GetRealTime() - real_start_mapping_time << "s.\n";
  OutputMappingStatistics();
  OutputMappingStatistics(num_reference_sequences, mappings_on_diff_ref_seqs_, mappings_on_diff_ref_seqs_);
  if (Tn5_shift_) {
    ApplyTn5ShiftOnPairedEndMapping(num_reference_sequences, &mappings_on_diff_ref_seqs_);
  }
  if (remove_pcr_duplicates_) {
    RemovePCRDuplicate(num_reference_sequences);
    std::cerr << "After removing PCR duplications, ";
    OutputMappingStatistics(num_reference_sequences, deduped_mappings_on_diff_ref_seqs_, deduped_mappings_on_diff_ref_seqs_);
  }
  if (allocate_multi_mappings_) {
    AllocateMultiMappings(num_reference_sequences);
    std::cerr << "After allocating multi-mappings, ";
    OutputMappingStatistics(num_reference_sequences, allocated_mappings_on_diff_ref_seqs_, allocated_mappings_on_diff_ref_seqs_);
    SortOutputMappings(num_reference_sequences, &allocated_mappings_on_diff_ref_seqs_);
    OutputPairedEndMappings(num_reference_sequences, reference, allocated_mappings_on_diff_ref_seqs_);
  } else {
    std::vector<std::vector<MappingRecord> > &mappings = remove_pcr_duplicates_ ? deduped_mappings_on_diff_ref_seqs_ : mappings_on_diff_ref_seqs_;
    OutputPairedEndMappings(num_reference_sequences, reference, mappings);
  }
  output_tools->FinalizeMappingOutput();
  reference.FinalizeLoading();
  std::cerr << "Total time: " << Chromap<>::GetRealTime() - real_start_time << "s.\n";
}

template <typename MappingRecord>
void Chromap<MappingRecord>::OutputPairedEndMappingsInVector(uint8_t mapq_threshold, uint32_t num_reference_sequences, const SequenceBatch &reference, const std::vector<std::vector<MappingRecord> > &mappings) {
  for (uint32_t ri = 0; ri < num_reference_sequences; ++ri) {
    for (auto it = mappings[ri].begin(); it != mappings[ri].end(); ++it) {
      uint8_t mapq = (it->mapq) >> 1;
      if (mapq >= mapq_threshold) {
        output_tools->AppendMapping(ri, reference, *it);
      }
    }
  }
}

template <typename MappingRecord>
void Chromap<MappingRecord>::OutputPairedEndMappings(uint32_t num_reference_sequences, const SequenceBatch &reference, const std::vector<std::vector<MappingRecord> > &mappings) {
  uint8_t mapq_threshold = only_output_unique_mappings_ ? 30 : 0;
  OutputPairedEndMappingsInVector(mapq_threshold, num_reference_sequences, reference, mappings);
}

template <typename MappingRecord>
void Chromap<MappingRecord>::ReduceCandidatesForPairedEndReadOnOneDirection(const std::vector<uint64_t> &candidates1, const std::vector<uint64_t> &candidates2, std::vector<uint64_t> *filtered_candidates1, std::vector<uint64_t> *filtered_candidates2) {
  uint32_t i1 = 0;
  uint32_t i2 = 0;
  uint32_t mapping_positions_distance = max_insert_size_;
  uint32_t previous_end_i2 = i2;
  while (i1 < candidates1.size() && i2 < candidates2.size()) {
    if (candidates1[i1] > candidates2[i2] + mapping_positions_distance) {
      ++i2;
    } else if (candidates2[i2] > candidates1[i1] + mapping_positions_distance) {
      ++i1;
    } else {
      // ok, find a pair, we store current ni2 somewhere and keep looking until we go out of the range, 
      // then we go back and then move to next pi1 and keep doing the similar thing. 
      filtered_candidates1->emplace_back(candidates1[i1]);
      uint32_t current_i2 = i2;
      while (current_i2 < candidates2.size() && candidates2[current_i2] <= candidates1[i1] + mapping_positions_distance) {
        if (current_i2 >= previous_end_i2) {
          filtered_candidates2->emplace_back(candidates2[current_i2]);
        }
        ++current_i2;
      }
      previous_end_i2 = current_i2;
      ++i1;
    }
  }
}

template <typename MappingRecord>
void Chromap<MappingRecord>::ReduceCandidatesForPairedEndRead(const std::vector<uint64_t> &positive_candidates1, const std::vector<uint64_t> &negative_candidates1, const std::vector<uint64_t> &positive_candidates2, const std::vector<uint64_t> &negative_candidates2, std::vector<uint64_t> *filtered_positive_candidates1, std::vector<uint64_t> *filtered_negative_candidates1, std::vector<uint64_t> *filtered_positive_candidates2, std::vector<uint64_t> *filtered_negative_candidates2) {
  ReduceCandidatesForPairedEndReadOnOneDirection(positive_candidates1, negative_candidates2, filtered_positive_candidates1, filtered_negative_candidates2);
  ReduceCandidatesForPairedEndReadOnOneDirection(negative_candidates1, positive_candidates2, filtered_negative_candidates1, filtered_positive_candidates2);
}

template <typename MappingRecord>
void Chromap<MappingRecord>::RecalibrateBestMappingsForPairedEndReadOnOneDirection(Direction first_read_direction, uint32_t pair_index, int min_sum_errors, int second_min_sum_errors, int min_num_errors1, int num_best_mappings1, int second_min_num_errors1, int num_second_best_mappings1, const SequenceBatch &read_batch1, const std::vector<std::pair<int, uint64_t> > &mappings1, int min_num_errors2, int num_best_mappings2, int second_min_num_errors2, int num_second_best_mappings2, const SequenceBatch &read_batch2, const SequenceBatch &reference, const std::vector<std::pair<int, uint64_t> > &mappings2, const std::vector<std::pair<uint32_t, uint32_t> > &edit_best_mappings, std::vector<std::pair<uint32_t, uint32_t> > *best_mappings, int *best_alignment_score, int *num_best_mappings, int *second_best_alignment_score, int *num_second_best_mappings) {
  int8_t mat[25];
  int i, j, k;
  for (i = k = 0; i < 4; ++i) {
    for (j = 0; j < 4; ++j)
      mat[k++] = i == j? match_score_ : -mismatch_penalty_;
    mat[k++] = 0; // ambiguous base
  }
  for (j = 0; j < 5; ++j) mat[k++] = 0;
  best_mappings->reserve(*num_best_mappings);
  const char *read1 = read_batch1.GetSequenceAt(pair_index);
  const char *read2 = read_batch2.GetSequenceAt(pair_index);
  uint32_t read1_length = read_batch1.GetSequenceLengthAt(pair_index);
  uint32_t read2_length = read_batch2.GetSequenceLengthAt(pair_index);
  const std::string &negative_read1 = read_batch1.GetNegativeSequenceAt(pair_index);
  const std::string &negative_read2 = read_batch2.GetNegativeSequenceAt(pair_index);
  //uint32_t read_id = read_batch1.GetSequenceIdAt(pair_index);
  for (uint32_t mi = 0; mi < edit_best_mappings.size(); ++mi) {
    uint32_t i1 = edit_best_mappings[mi].first;
    uint32_t i2 = edit_best_mappings[mi].second;
    int current_sum_errors = mappings1[i1].first + mappings2[i2].first;
    if (current_sum_errors == min_sum_errors) {
      uint32_t rid1 = mappings1[i1].second >> 32;
      uint32_t position1 = mappings1[i1].second;
      uint32_t verification_window_start_position1 = position1 + 1 > (read1_length + error_threshold_) ? position1 + 1 - read1_length - error_threshold_ : 0;
      if (position1 >= reference.GetSequenceLengthAt(rid1)) {
        verification_window_start_position1 = reference.GetSequenceLengthAt(rid1) - error_threshold_ - read1_length; 
      }
      //int mapping_start_position1;
      uint32_t rid2 = mappings2[i2].second >> 32;
      uint32_t position2 = mappings2[i2].second;
      uint32_t verification_window_start_position2 = position2 + 1 > (read2_length + error_threshold_) ? position2 + 1 - read2_length - error_threshold_ : 0;
      if (position2 >= reference.GetSequenceLengthAt(rid2)) {
        verification_window_start_position2 = reference.GetSequenceLengthAt(rid2) - error_threshold_ - read2_length; 
      }
      int current_alignment_score1, current_alignment_score2, current_alignment_score; 
      if (first_read_direction == kPositive) {
        current_alignment_score1 = ksw_semi_global2(read1_length + 2 * error_threshold_, reference.GetSequenceAt(rid1) + verification_window_start_position1, read1_length, read1, 5, mat, gap_open_penalties_[0], gap_extension_penalties_[0], gap_open_penalties_[1], gap_extension_penalties_[1], error_threshold_ * 2 + 1, NULL, NULL);
        current_alignment_score2 = ksw_semi_global2(read2_length + 2 * error_threshold_, reference.GetSequenceAt(rid2) + verification_window_start_position2, read2_length, negative_read2.data(), 5, mat, gap_open_penalties_[0], gap_extension_penalties_[0], gap_open_penalties_[1], gap_extension_penalties_[1], error_threshold_ * 2 + 1, NULL, NULL);
      } else {
        current_alignment_score1 = ksw_semi_global2(read1_length + 2 * error_threshold_, reference.GetSequenceAt(rid1) + verification_window_start_position1, read1_length, negative_read1.data(), 5, mat, gap_open_penalties_[0], gap_extension_penalties_[0], gap_open_penalties_[1], gap_extension_penalties_[1], error_threshold_ * 2 + 1, NULL, NULL);
        current_alignment_score2 = ksw_semi_global2(read1_length + 2 * error_threshold_, reference.GetSequenceAt(rid2) + verification_window_start_position2, read2_length, read2, 5, mat, gap_open_penalties_[0], gap_extension_penalties_[0], gap_open_penalties_[1], gap_extension_penalties_[1], error_threshold_ * 2 + 1, NULL, NULL);
      }
      current_alignment_score = current_alignment_score1 + current_alignment_score2;
      //std::cerr << current_alignment_score1 << " " << current_alignment_score2 << " " << current_alignment_score <<"\n";
      if (current_alignment_score > *best_alignment_score) {
        *second_best_alignment_score = *best_alignment_score;
        *num_second_best_mappings = *num_best_mappings;
        *best_alignment_score = current_alignment_score;
        *num_best_mappings = 1;
        best_mappings->clear();
        best_mappings->emplace_back(i1, i2);
      } else if (current_alignment_score == *best_alignment_score) {
        (*num_best_mappings)++;
        best_mappings->emplace_back(i1, i2);
      } else if (current_alignment_score == *second_best_alignment_score) {
        (*num_second_best_mappings)++;
      }
    } else if (current_sum_errors == second_min_sum_errors) {
    }
  }
}
 
template <typename MappingRecord>
void Chromap<MappingRecord>::GenerateBestMappingsForPairedEndReadOnOneDirection(Direction first_read_direction, uint32_t pair_index, int num_candidates1, int min_num_errors1, int num_best_mappings1, int second_min_num_errors1, int num_second_best_mappings1, const SequenceBatch &read_batch1, const std::vector<std::pair<int, uint64_t> > &mappings1, int num_candidates2, int min_num_errors2, int num_best_mappings2, int second_min_num_errors2, int num_second_best_mappings2, const SequenceBatch &read_batch2, const SequenceBatch &reference, const std::vector<std::pair<int, uint64_t> > &mappings2, std::vector<std::pair<uint32_t, uint32_t> > *best_mappings, int *min_sum_errors, int *num_best_mappings, int *second_min_sum_errors, int *num_second_best_mappings) {
  uint32_t i1 = 0;
  uint32_t i2 = 0;
  uint32_t min_overlap_length = min_read_length_;
  uint32_t read1_length = read_batch1.GetSequenceLengthAt(pair_index);
  uint32_t read2_length = read_batch2.GetSequenceLengthAt(pair_index);
  while (i1 < mappings1.size() && i2 < mappings2.size()) {
    if ((first_read_direction == kNegative && mappings1[i1].second > mappings2[i2].second + max_insert_size_ - read1_length) || (first_read_direction == kPositive && mappings1[i1].second > mappings2[i2].second + read2_length - min_overlap_length)) {
      ++i2;
    } else if ((first_read_direction == kPositive && mappings2[i2].second > mappings1[i1].second + max_insert_size_ - read2_length) || (first_read_direction == kNegative && mappings2[i2].second > mappings1[i1].second + read1_length - min_overlap_length)) {
      ++i1;
    } else {
      // ok, find a pair, we store current ni2 somewhere and keep looking until we go out of the range, 
      // then we go back and then move to next pi1 and keep doing the similar thing. 
      uint32_t current_i2 = i2;
      while (current_i2 < mappings2.size() && ((first_read_direction == kPositive && mappings2[current_i2].second <= mappings1[i1].second + max_insert_size_ - read2_length) || (first_read_direction == kNegative && mappings2[current_i2].second <= mappings1[i1].second + read1_length - min_overlap_length))) {
        int current_sum_errors = mappings1[i1].first + mappings2[current_i2].first;
        if (current_sum_errors < *min_sum_errors) {
          *second_min_sum_errors = *min_sum_errors;
          *num_second_best_mappings = *num_best_mappings;
          *min_sum_errors = current_sum_errors;
          *num_best_mappings = 1;
          best_mappings->emplace_back(i1, current_i2);
        } else if (current_sum_errors == *min_sum_errors) {
          (*num_best_mappings)++;
          best_mappings->emplace_back(i1, current_i2);
        } else if (current_sum_errors == *second_min_sum_errors) {
          (*num_second_best_mappings)++;
        }
        ++current_i2;
      }
      ++i1;
    }
  }
}

template<typename MappingRecord>
void Chromap<MappingRecord>::EmplaceBackMappingRecord(uint32_t read_id, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, uint16_t positive_alignment_length, uint16_t negative_alignment_length, std::vector<MappingRecord> *mappings_on_diff_ref_seqs) {
}

template<>
void Chromap<PairedEndMappingWithoutBarcode>::EmplaceBackMappingRecord(uint32_t read_id, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, uint16_t positive_alignment_length, uint16_t negative_alignment_length, std::vector<PairedEndMappingWithoutBarcode> *mappings_on_diff_ref_seqs) {
  mappings_on_diff_ref_seqs->emplace_back(PairedEndMappingWithoutBarcode{read_id, fragment_start_position, fragment_length, mapq, positive_alignment_length, negative_alignment_length});
}

template<>
void Chromap<PairedEndMappingWithBarcode>::EmplaceBackMappingRecord(uint32_t read_id, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, uint16_t positive_alignment_length, uint16_t negative_alignment_length, std::vector<PairedEndMappingWithBarcode> *mappings_on_diff_ref_seqs) {
  mappings_on_diff_ref_seqs->emplace_back(PairedEndMappingWithBarcode{read_id, barcode, fragment_start_position, fragment_length, mapq, positive_alignment_length, negative_alignment_length});
}

template<typename MappingRecord>
void Chromap<MappingRecord>::EmplaceBackMappingRecord(uint32_t read_id, const char *read1_name, const char *read2_name, uint16_t read1_length, uint16_t read2_length, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, uint16_t positive_alignment_length, uint16_t negative_alignment_length, std::vector<MappingRecord> *mappings_on_diff_ref_seqs) {
}

template<>
void Chromap<PairedPAFMapping>::EmplaceBackMappingRecord(uint32_t read_id, const char *read1_name, const char *read2_name, uint16_t read1_length, uint16_t read2_length, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, uint16_t positive_alignment_length, uint16_t negative_alignment_length, std::vector<PairedPAFMapping> *mappings_on_diff_ref_seqs) {
  mappings_on_diff_ref_seqs->emplace_back(PairedPAFMapping{read_id, std::string(read1_name), std::string(read2_name), read1_length, read2_length, fragment_start_position, fragment_length, positive_alignment_length, negative_alignment_length, mapq});
}

template <typename MappingRecord>
void Chromap<MappingRecord>::ProcessBestMappingsForPairedEndReadOnOneDirection(Direction first_read_direction, uint32_t pair_index, uint8_t mapq, int num_candidates1, int min_num_errors1, int num_best_mappings1, int second_min_num_errors1, int num_second_best_mappings1, const SequenceBatch &read_batch1, const std::vector<std::pair<int, uint64_t> > &mappings1, int num_candidates2, int min_num_errors2, int num_best_mappings2, int second_min_num_errors2, int num_second_best_mappings2, const SequenceBatch &read_batch2, const SequenceBatch &reference, const SequenceBatch &barcode_batch, const std::vector<int> &best_mapping_indices, const std::vector<std::pair<int, uint64_t> > &mappings2, const std::vector<std::pair<uint32_t, uint32_t> > &best_mappings, int min_sum_errors, int num_best_mappings, int second_min_sum_errors, int num_second_best_mappings, int *best_mapping_index, int *num_best_mappings_reported, std::vector<std::vector<MappingRecord> > *mappings_on_diff_ref_seqs) {
  const char *read1 = read_batch1.GetSequenceAt(pair_index);
  const char *read2 = read_batch2.GetSequenceAt(pair_index);
  uint32_t read1_length = read_batch1.GetSequenceLengthAt(pair_index);
  uint32_t read2_length = read_batch2.GetSequenceLengthAt(pair_index);
  const std::string &negative_read1 = read_batch1.GetNegativeSequenceAt(pair_index);
  const std::string &negative_read2 = read_batch2.GetNegativeSequenceAt(pair_index);
  uint32_t read_id = read_batch1.GetSequenceIdAt(pair_index);
  for (uint32_t mi = 0; mi < best_mappings.size(); ++mi) {
    uint32_t i1 = best_mappings[mi].first;
    uint32_t i2 = best_mappings[mi].second;
    int current_sum_errors = mappings1[i1].first + mappings2[i2].first;
    if (current_sum_errors == min_sum_errors) {
      if (*best_mapping_index == best_mapping_indices[*num_best_mappings_reported]) {
        uint32_t rid1 = mappings1[i1].second >> 32;
        uint32_t position1 = mappings1[i1].second;
        //uint32_t verification_window_start_position1 = position1 + 1 - read1_length - error_threshold_;
        uint32_t verification_window_start_position1 = position1 + 1 > (read1_length + error_threshold_) ? position1 + 1 - read1_length - error_threshold_ : 0;
        if (position1 >= reference.GetSequenceLengthAt(rid1)) {
          verification_window_start_position1 = reference.GetSequenceLengthAt(rid1) - error_threshold_ - read1_length; 
        }
        int mapping_start_position1;
        uint32_t rid2 = mappings2[i2].second >> 32;
        uint32_t position2 = mappings2[i2].second;
        //uint32_t verification_window_start_position2 = position2 + 1 - read2_length - error_threshold_;
        uint32_t verification_window_start_position2 = position2 + 1 > (read2_length + error_threshold_) ? position2 + 1 - read2_length - error_threshold_ : 0;
        if (position2 >= reference.GetSequenceLengthAt(rid2)) {
          verification_window_start_position2 = reference.GetSequenceLengthAt(rid2) - error_threshold_ - read2_length; 
        }
        int mapping_start_position2;
        if (first_read_direction == kPositive) {
          BandedTraceback(mappings1[i1].first, reference.GetSequenceAt(rid1) + verification_window_start_position1, read1, read1_length, &mapping_start_position1);
          BandedTraceback(mappings2[i2].first, reference.GetSequenceAt(rid2) + verification_window_start_position2, negative_read2.data(), read2_length, &mapping_start_position2);
          uint32_t fragment_start_position = verification_window_start_position1 + mapping_start_position1;
          uint16_t fragment_length = position2 - fragment_start_position + 1;
          uint16_t positive_alignment_length = position1 + 1 - fragment_start_position;
          uint16_t negative_alignment_length = position2 + 1 - (verification_window_start_position2 + mapping_start_position2);
          mapq = GetMAPQ(num_candidates1, num_candidates2, positive_alignment_length + negative_alignment_length, min_sum_errors, num_best_mappings, second_min_sum_errors, num_second_best_mappings);
          mapq |= (uint8_t)1;
          uint32_t barcode_key = 0;
          if (!is_bulk_data_) {
            barcode_key = barcode_batch.GenerateSeedFromSequenceAt(pair_index, 0, barcode_batch.GetSequenceLengthAt(pair_index));
          }
          if (output_mapping_in_PAF_) {
            EmplaceBackMappingRecord(read_id, read_batch1.GetSequenceNameAt(pair_index), read_batch2.GetSequenceNameAt(pair_index), (uint16_t)read_batch1.GetSequenceLengthAt(pair_index), (uint16_t)read_batch2.GetSequenceLengthAt(pair_index), barcode_key, fragment_start_position, fragment_length, mapq, positive_alignment_length, negative_alignment_length, &((*mappings_on_diff_ref_seqs)[rid1]));
          } else {
            EmplaceBackMappingRecord(read_id, barcode_key, fragment_start_position, fragment_length, mapq, positive_alignment_length, negative_alignment_length, &((*mappings_on_diff_ref_seqs)[rid1]));
          }
        } else {
          BandedTraceback(mappings1[i1].first, reference.GetSequenceAt(rid1) + verification_window_start_position1, negative_read1.data(), read1_length, &mapping_start_position1);
          BandedTraceback(mappings2[i2].first, reference.GetSequenceAt(rid2) + verification_window_start_position2, read2, read2_length, &mapping_start_position2);
          uint32_t fragment_start_position = verification_window_start_position2 + mapping_start_position2;
          uint16_t fragment_length = position1 - fragment_start_position + 1;
          uint16_t positive_alignment_length = position2 + 1 - fragment_start_position;
          uint16_t negative_alignment_length = position1 + 1 - (verification_window_start_position1 + mapping_start_position1);
          mapq = GetMAPQ(num_candidates1, num_candidates2, positive_alignment_length + negative_alignment_length, min_sum_errors, num_best_mappings, second_min_sum_errors, num_second_best_mappings);
          uint32_t barcode_key = 0;
          if (!is_bulk_data_) {
            barcode_key = barcode_batch.GenerateSeedFromSequenceAt(pair_index, 0, barcode_batch.GetSequenceLengthAt(pair_index));
          }
          if (output_mapping_in_PAF_) {
            EmplaceBackMappingRecord(read_id, read_batch1.GetSequenceNameAt(pair_index), read_batch2.GetSequenceNameAt(pair_index), (uint16_t)read_batch1.GetSequenceLengthAt(pair_index), (uint16_t)read_batch2.GetSequenceLengthAt(pair_index), barcode_key, fragment_start_position, fragment_length, mapq, positive_alignment_length, negative_alignment_length, &((*mappings_on_diff_ref_seqs)[rid1]));
          } else {
            EmplaceBackMappingRecord(read_id, barcode_key, fragment_start_position, fragment_length, mapq, positive_alignment_length, negative_alignment_length, &((*mappings_on_diff_ref_seqs)[rid1]));
          }
        }
        (*num_best_mappings_reported)++;
        if (*num_best_mappings_reported == std::min(max_num_best_mappings_, num_best_mappings)) {
          break;
        }
      }
      (*best_mapping_index)++;
    }
  }
}

template <typename MappingRecord>
void Chromap<MappingRecord>::GenerateBestMappingsForPairedEndRead(uint32_t pair_index, int num_positive_candidates1, int num_negative_candidates1, int min_num_errors1, int num_best_mappings1, int second_min_num_errors1, int num_second_best_mappings1, const SequenceBatch &read_batch1, const std::vector<std::pair<int, uint64_t> > &positive_mappings1, const std::vector<std::pair<int, uint64_t> > &negative_mappings1, int num_positive_candidates2, int num_negative_candidates2, int min_num_errors2, int num_best_mappings2, int second_min_num_errors2, int num_second_best_mappings2, const SequenceBatch &read_batch2, const SequenceBatch &reference, const SequenceBatch &barcode_batch, const std::vector<std::pair<int, uint64_t> > &positive_mappings2, const std::vector<std::pair<int, uint64_t> > &negative_mappings2, std::vector<int> *best_mapping_indices, std::mt19937 *generator, std::vector<std::pair<uint32_t, uint32_t> > *F1R2_best_mappings, std::vector<std::pair<uint32_t, uint32_t> > *F2R1_best_mappings, int *min_sum_errors, int *num_best_mappings, int *second_min_sum_errors, int *num_second_best_mappings, std::vector<std::vector<MappingRecord> > *mappings_on_diff_ref_seqs) {
  *min_sum_errors = 2 * error_threshold_ + 1;
  *num_best_mappings = 0;
  *second_min_sum_errors = *min_sum_errors;
  *num_second_best_mappings = 0;
  GenerateBestMappingsForPairedEndReadOnOneDirection(kPositive, pair_index, num_positive_candidates1, min_num_errors1, num_best_mappings1, second_min_num_errors1, num_second_best_mappings1, read_batch1, positive_mappings1, num_negative_candidates2, min_num_errors2, num_best_mappings2, second_min_num_errors2, num_second_best_mappings2, read_batch2, reference, negative_mappings2, F1R2_best_mappings, min_sum_errors, num_best_mappings, second_min_sum_errors, num_second_best_mappings);
  GenerateBestMappingsForPairedEndReadOnOneDirection(kNegative, pair_index, num_negative_candidates1, min_num_errors1, num_best_mappings1, second_min_num_errors1, num_second_best_mappings1, read_batch1, negative_mappings1, num_positive_candidates2, min_num_errors2, num_best_mappings2, second_min_num_errors2, num_second_best_mappings2, read_batch2, reference, positive_mappings2, F2R1_best_mappings, min_sum_errors, num_best_mappings, second_min_sum_errors, num_second_best_mappings);
  //int best_alignment_score, second_best_alignment_score;
  //best_alignment_score = -1;
  //*num_best_mappings = 0;
  //second_best_alignment_score = best_alignment_score;
  //*num_second_best_mappings = 0;
  //std::vector<std::pair<uint32_t, uint32_t> > edit_F1R2_best_mappings, edit_F2R1_best_mappings;
  //edit_F1R2_best_mappings.swap(*F1R2_best_mappings);
  //edit_F2R1_best_mappings.swap(*F2R1_best_mappings);
  //RecalibrateBestMappingsForPairedEndReadOnOneDirection(kPositive, pair_index, *min_sum_errors, *second_min_sum_errors, min_num_errors1, num_best_mappings1, second_min_num_errors1, num_second_best_mappings1, read_batch1, positive_mappings1, min_num_errors2, num_best_mappings2, second_min_num_errors2, num_second_best_mappings2, read_batch2, reference, negative_mappings2, edit_F1R2_best_mappings, F1R2_best_mappings, &best_alignment_score, num_best_mappings, &second_best_alignment_score, num_second_best_mappings);
  //RecalibrateBestMappingsForPairedEndReadOnOneDirection(kNegative, pair_index, *min_sum_errors, *second_min_sum_errors, min_num_errors1, num_best_mappings1, second_min_num_errors1, num_second_best_mappings1, read_batch1, negative_mappings1, min_num_errors2, num_best_mappings2, second_min_num_errors2, num_second_best_mappings2, read_batch2, reference, positive_mappings2, edit_F2R1_best_mappings, F2R1_best_mappings, &best_alignment_score, num_best_mappings, &second_best_alignment_score, num_second_best_mappings);
  //uint8_t mapq = GetMAPQ(*num_best_mappings, *num_second_best_mappings);
  uint8_t mapq = 0;
  if (*num_best_mappings <= drop_repetitive_reads_) { 
    // we will use reservoir sampling 
    //std::vector<int> best_mapping_indices(max_num_best_mappings_);
    std::iota(best_mapping_indices->begin(), best_mapping_indices->end(), 0);
    if (*num_best_mappings > max_num_best_mappings_) {
      //std::mt19937 tmp_generator(11);
      for (int i = max_num_best_mappings_; i < *num_best_mappings; ++i) {
        std::uniform_int_distribution<int> distribution(0, i); // important: inclusive range
        int j = distribution(*generator); 
        //int j = distribution(tmp_generator); 
        if (j < max_num_best_mappings_) {
          (*best_mapping_indices)[j] = i;
        }
      }
      std::sort(best_mapping_indices->begin(), best_mapping_indices->end());
    }
    int best_mapping_index = 0;
    int num_best_mappings_reported = 0;
    ProcessBestMappingsForPairedEndReadOnOneDirection(kPositive, pair_index, mapq, num_positive_candidates1, min_num_errors1, num_best_mappings1, second_min_num_errors1, num_second_best_mappings1, read_batch1, positive_mappings1, num_negative_candidates2, min_num_errors2, num_best_mappings2, second_min_num_errors2, num_second_best_mappings2, read_batch2, reference, barcode_batch, *best_mapping_indices, negative_mappings2, *F1R2_best_mappings, *min_sum_errors, *num_best_mappings, *second_min_sum_errors, *num_second_best_mappings, &best_mapping_index, &num_best_mappings_reported, mappings_on_diff_ref_seqs);
    if (num_best_mappings_reported != std::min(max_num_best_mappings_, *num_best_mappings)) {
      ProcessBestMappingsForPairedEndReadOnOneDirection(kNegative, pair_index, mapq, num_negative_candidates1, min_num_errors1, num_best_mappings1, second_min_num_errors1, num_second_best_mappings1, read_batch1, negative_mappings1, num_positive_candidates2, min_num_errors2, num_best_mappings2, second_min_num_errors2, num_second_best_mappings2, read_batch2, reference, barcode_batch, *best_mapping_indices, positive_mappings2, *F2R1_best_mappings, *min_sum_errors, *num_best_mappings, *second_min_sum_errors, *num_second_best_mappings, &best_mapping_index, &num_best_mappings_reported, mappings_on_diff_ref_seqs);
    }
  }
}

template <typename MappingRecord>
void Chromap<MappingRecord>::ApplyTn5ShiftOnPairedEndMapping(uint32_t num_reference_sequences, std::vector<std::vector<MappingRecord> > *mappings) {
  uint64_t num_shifted_mappings = 0;
  for (auto &mappings_on_one_ref_seq : *mappings) {
    for(auto &mapping: mappings_on_one_ref_seq) {
      mapping.fragment_start_position += 4;
      mapping.positive_alignment_length -= 4;
      mapping.fragment_length -= 9;
      mapping.negative_alignment_length -= 5;
      ++num_shifted_mappings;
    }
  } 
  std::cerr << "# shifted mappings: " << num_shifted_mappings << ".\n";
}

template <typename MappingRecord>
void Chromap<MappingRecord>::MapSingleEndReads() {
  // TODO(Haowen): check args for single-end read mapping
  double real_start_time = Chromap<>::GetRealTime();
  SequenceBatch reference;
  reference.InitializeLoading(reference_file_path_);
  uint32_t num_reference_sequences = reference.LoadAllSequences();
  Index index(min_num_seeds_required_for_mapping_, max_seed_frequencies_, index_file_path_);
  index.Load();
  //index.Statistics(num_sequences, reference);
  SequenceBatch read_batch(read_batch_size_);
  SequenceBatch read_batch_for_loading(read_batch_size_);
  SequenceBatch barcode_batch(read_batch_size_);
  SequenceBatch barcode_batch_for_loading(read_batch_size_);
  read_batch_for_loading.InitializeLoading(read_file1_path_);
  if (!is_bulk_data_) {
    barcode_batch_for_loading.InitializeLoading(barcode_file_path_);
  }
  double real_start_mapping_time = Chromap<>::GetRealTime();
  uint32_t num_loaded_reads_for_loading = 0;
  uint32_t num_loaded_reads = LoadSingleEndReadsWithBarcodes(&read_batch_for_loading, &barcode_batch_for_loading);
  read_batch_for_loading.SwapSequenceBatch(read_batch);
  barcode_batch_for_loading.SwapSequenceBatch(barcode_batch);
  mappings_on_diff_ref_seqs_.reserve(num_reference_sequences);
  deduped_mappings_on_diff_ref_seqs_.reserve(num_reference_sequences);
  for (uint32_t i = 0; i < num_reference_sequences; ++i) {
    mappings_on_diff_ref_seqs_.emplace_back(std::vector<MappingRecord>());
    deduped_mappings_on_diff_ref_seqs_.emplace_back(std::vector<MappingRecord>());
  }
  std::vector<std::vector<std::vector<MappingRecord> > > mappings_on_diff_ref_seqs_for_diff_threads;
  std::vector<std::vector<std::vector<MappingRecord> > > mappings_on_diff_ref_seqs_for_diff_threads_for_saving;
  mappings_on_diff_ref_seqs_for_diff_threads.reserve(num_threads_);
  mappings_on_diff_ref_seqs_for_diff_threads_for_saving.reserve(num_threads_);
  for (int ti = 0; ti < num_threads_; ++ti) {
    mappings_on_diff_ref_seqs_for_diff_threads.emplace_back(std::vector<std::vector<MappingRecord> >(num_reference_sequences));
    mappings_on_diff_ref_seqs_for_diff_threads_for_saving.emplace_back(std::vector<std::vector<MappingRecord> >(num_reference_sequences));
    for (uint32_t i = 0; i < num_reference_sequences; ++i) {
      mappings_on_diff_ref_seqs_for_diff_threads[ti][i].reserve((num_loaded_reads + num_loaded_reads / 1000 * max_num_best_mappings_) / num_threads_ / num_reference_sequences);
      mappings_on_diff_ref_seqs_for_diff_threads_for_saving[ti][i].reserve((num_loaded_reads + num_loaded_reads / 1000 * max_num_best_mappings_) / num_threads_ / num_reference_sequences);
    }
  }
  if (output_mapping_in_BED_) {
    output_tools = std::unique_ptr<BEDOutputTools<MappingRecord> >(new BEDOutputTools<MappingRecord>);
  } else if (output_mapping_in_TagAlign_) {
    output_tools = std::unique_ptr<TagAlignOutputTools<MappingRecord> >(new TagAlignOutputTools<MappingRecord>);
  } else if (output_mapping_in_PAF_) {
    output_tools = std::unique_ptr<PAFOutputTools<MappingRecord> >(new PAFOutputTools<MappingRecord>);
  }
  output_tools->InitializeMappingOutput(mapping_output_file_path_);
  static uint64_t thread_num_candidates = 0;
  static uint64_t thread_num_mappings = 0;
  static uint64_t thread_num_mapped_reads = 0; 
  static uint64_t thread_num_uniquely_mapped_reads = 0; 
#pragma omp threadprivate(thread_num_candidates, thread_num_mappings, thread_num_mapped_reads, thread_num_uniquely_mapped_reads)
#pragma omp parallel default(none) shared(reference, index, read_batch, barcode_batch, read_batch_for_loading, barcode_batch_for_loading, std::cerr, num_loaded_reads_for_loading, num_loaded_reads, num_reference_sequences, mappings_on_diff_ref_seqs_for_diff_threads, mappings_on_diff_ref_seqs_for_diff_threads_for_saving) num_threads(num_threads_) reduction(+:num_candidates_, num_mappings_, num_mapped_reads_, num_uniquely_mapped_reads_)
  {
  std::vector<std::pair<uint64_t, uint64_t> > minimizers;
  std::vector<uint64_t> positive_hits;
  std::vector<uint64_t> negative_hits;
  positive_hits.reserve(max_seed_frequencies_[0]);
  negative_hits.reserve(max_seed_frequencies_[0]);
  std::vector<uint64_t> positive_candidates;
  std::vector<uint64_t> negative_candidates;
  positive_candidates.reserve(max_seed_frequencies_[0]);
  negative_candidates.reserve(max_seed_frequencies_[0]);
  std::vector<std::pair<int, uint64_t> > positive_mappings;
  std::vector<std::pair<int, uint64_t> > negative_mappings;
  positive_mappings.reserve(max_seed_frequencies_[0]);
  negative_mappings.reserve(max_seed_frequencies_[0]);
  #pragma omp single
  {
  while (num_loaded_reads > 0) {
    double real_batch_start_time = Chromap<>::GetRealTime();
    num_reads_ += num_loaded_reads;
#pragma omp task
    {
    num_loaded_reads_for_loading = LoadSingleEndReadsWithBarcodes(&read_batch_for_loading, &barcode_batch_for_loading);
    } // end of openmp loading task
    int grain_size = 10000;
#pragma omp taskloop grainsize(grain_size) //num_tasks(num_threads_* 50)
    for (uint32_t read_index = 0; read_index < num_loaded_reads; ++read_index) {
      read_batch.PrepareNegativeSequenceAt(read_index);
      //std::cerr << "Generated negative sequence!\n";
      minimizers.clear();
      minimizers.reserve(read_batch.GetSequenceLengthAt(read_index) / window_size_ * 2);
      index.GenerateMinimizerSketch(read_batch, read_index, &minimizers);
      if (minimizers.size() > 0) {
        //std::cerr << "Generated minimizers!\n";
        positive_hits.clear();
        negative_hits.clear();
        positive_candidates.clear();
        negative_candidates.clear();
        index.GenerateCandidates(minimizers, &positive_hits, &negative_hits, &positive_candidates, &negative_candidates);
        uint32_t current_num_candidates = positive_candidates.size() + negative_candidates.size(); 
        //std::cerr << "Generated candidates!\n";
        if (current_num_candidates > 0) {
          thread_num_candidates += current_num_candidates;
          positive_mappings.clear();
          negative_mappings.clear();
          int min_num_errors, second_min_num_errors;
          int num_best_mappings, num_second_best_mappings;
          VerifyCandidates(read_batch, read_index, reference, positive_candidates, negative_candidates, &positive_mappings, &negative_mappings, &min_num_errors, &num_best_mappings, &second_min_num_errors, &num_second_best_mappings);
          uint32_t current_num_mappings = positive_mappings.size() + negative_mappings.size();
          //std::cerr << "Verified candidates!\n";
          if (current_num_mappings > 0) {
            std::vector<std::vector<MappingRecord> > &mappings_on_diff_ref_seqs = mappings_on_diff_ref_seqs_for_diff_threads[omp_get_thread_num()];
            GenerateBestMappingsForSingleEndRead(min_num_errors, num_best_mappings, second_min_num_errors, num_second_best_mappings, read_batch, read_index, reference, barcode_batch, positive_mappings, negative_mappings, &mappings_on_diff_ref_seqs);
            thread_num_mappings += std::min(num_best_mappings, max_num_best_mappings_);
            //std::cerr << "Generated output!\n";
            ++thread_num_mapped_reads;
            if (num_best_mappings == 1) {
              ++thread_num_uniquely_mapped_reads;
            }
          }
        }
      }
    }
#pragma omp taskwait
    num_loaded_reads = num_loaded_reads_for_loading;
    read_batch_for_loading.SwapSequenceBatch(read_batch);
    barcode_batch_for_loading.SwapSequenceBatch(barcode_batch);
    mappings_on_diff_ref_seqs_for_diff_threads.swap(mappings_on_diff_ref_seqs_for_diff_threads_for_saving);
#pragma omp task
    {
    MoveMappingsInBuffersToMappingContainer(num_reference_sequences, &mappings_on_diff_ref_seqs_for_diff_threads_for_saving);
    }
    std::cerr << "Mapped in " << Chromap<>::GetRealTime() - real_batch_start_time << "s.\n";
  }
  } // end of openmp single
  {
    num_candidates_ += thread_num_candidates;
    num_mappings_ += thread_num_mappings;
    num_mapped_reads_ += thread_num_mapped_reads;
    num_uniquely_mapped_reads_ += thread_num_uniquely_mapped_reads;
  } // end of updating shared mapping stats
  } // end of openmp parallel region
  read_batch_for_loading.FinalizeLoading();
  if (!is_bulk_data_) {
    barcode_batch_for_loading.FinalizeLoading();
  }
  OutputMappingStatistics();
  std::cerr << "Mapped all reads in " << Chromap<>::GetRealTime() - real_start_mapping_time << "s.\n";
  OutputMappingStatistics(num_reference_sequences, mappings_on_diff_ref_seqs_, mappings_on_diff_ref_seqs_);
  if (Tn5_shift_) {
    ApplyTn5ShiftOnSingleEndMapping(num_reference_sequences, &mappings_on_diff_ref_seqs_);
  }
  if (remove_pcr_duplicates_) {
    RemovePCRDuplicate(num_reference_sequences);
    std::cerr << "After removing PCR duplications, ";
    OutputMappingStatistics(num_reference_sequences, deduped_mappings_on_diff_ref_seqs_, deduped_mappings_on_diff_ref_seqs_);
  }
  if (allocate_multi_mappings_) {
    AllocateMultiMappings(num_reference_sequences);
    std::cerr << "After allocating multi-mappings, ";
    OutputMappingStatistics(num_reference_sequences, allocated_mappings_on_diff_ref_seqs_, allocated_mappings_on_diff_ref_seqs_);
    SortOutputMappings(num_reference_sequences, &allocated_mappings_on_diff_ref_seqs_);
    OutputPairedEndMappings(num_reference_sequences, reference, allocated_mappings_on_diff_ref_seqs_);
  } else {
    std::vector<std::vector<MappingRecord> > &mappings = remove_pcr_duplicates_ ? deduped_mappings_on_diff_ref_seqs_ : mappings_on_diff_ref_seqs_;
    OutputPairedEndMappings(num_reference_sequences, reference, mappings);
  }
  output_tools->FinalizeMappingOutput();
  reference.FinalizeLoading();
  std::cerr << "Total time: " << Chromap<>::GetRealTime() - real_start_time << "s.\n";
}

template<typename MappingRecord>
void Chromap<MappingRecord>::EmplaceBackMappingRecord(uint32_t read_id, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, std::vector<MappingRecord> *mappings_on_diff_ref_seqs) {
}

template<>
void Chromap<MappingWithoutBarcode>::EmplaceBackMappingRecord(uint32_t read_id, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, std::vector<MappingWithoutBarcode> *mappings_on_diff_ref_seqs) {
  mappings_on_diff_ref_seqs->emplace_back(MappingWithoutBarcode{read_id, fragment_start_position, fragment_length, mapq});
}

template<>
void Chromap<MappingWithBarcode>::EmplaceBackMappingRecord(uint32_t read_id, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, std::vector<MappingWithBarcode> *mappings_on_diff_ref_seqs) {
  mappings_on_diff_ref_seqs->emplace_back(MappingWithBarcode{read_id, barcode, fragment_start_position, fragment_length, mapq});
}

template<typename MappingRecord>
void Chromap<MappingRecord>::EmplaceBackMappingRecord(uint32_t read_id, const char *read_name, uint16_t read_length, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, std::vector<MappingRecord> *mappings_on_diff_ref_seqs) {
}

template<>
void Chromap<PAFMapping>::EmplaceBackMappingRecord(uint32_t read_id, const char *read_name, uint16_t read_length, uint32_t barcode, uint32_t fragment_start_position, uint16_t fragment_length, uint8_t mapq, std::vector<PAFMapping> *mappings_on_diff_ref_seqs) {
  mappings_on_diff_ref_seqs->emplace_back(PAFMapping{read_id, std::string(read_name), read_length, fragment_start_position, fragment_length, mapq});
}

template <typename MappingRecord>
void Chromap<MappingRecord>::ProcessBestMappingsForSingleEndRead(Direction mapping_direction, uint8_t mapq, int min_num_errors, int num_best_mappings, int second_min_num_errors, int num_second_best_mappings, const SequenceBatch &read_batch, uint32_t read_index, const SequenceBatch &reference, const SequenceBatch &barcode_batch, const std::vector<int> &best_mapping_indices, const std::vector<std::pair<int, uint64_t> > &mappings, int *best_mapping_index, int *num_best_mappings_reported, std::vector<std::vector<MappingRecord> > *mappings_on_diff_ref_seqs) {
  const char *read = read_batch.GetSequenceAt(read_index);
  uint32_t read_id = read_batch.GetSequenceIdAt(read_index);
  const char *read_name = read_batch.GetSequenceNameAt(read_index);
  uint32_t read_length = read_batch.GetSequenceLengthAt(read_index);
  const std::string &negative_read = read_batch.GetNegativeSequenceAt(read_index);
  for (uint32_t mi = 0; mi < mappings.size(); ++mi) {
    if (mappings[mi].first == min_num_errors) {
      if (*best_mapping_index == best_mapping_indices[*num_best_mappings_reported]) {
        uint32_t rid = mappings[mi].second >> 32;
        uint32_t position = mappings[mi].second;
        uint32_t verification_window_start_position = position + 1 > (read_length + error_threshold_) ? position + 1 - read_length - error_threshold_ : 0;
        if (position >= reference.GetSequenceLengthAt(rid)) {
          verification_window_start_position = reference.GetSequenceLengthAt(rid) - error_threshold_ - read_length; 
        }
        uint32_t barcode_key = 0;
        if (!is_bulk_data_) {
          barcode_key = barcode_batch.GenerateSeedFromSequenceAt(read_index, 0, barcode_batch.GetSequenceLengthAt(read_index));
        }
        int mapping_start_position;
        if (mapping_direction == kPositive) { 
          BandedTraceback(min_num_errors, reference.GetSequenceAt(rid) + verification_window_start_position, read, read_length, &mapping_start_position);
          uint32_t fragment_start_position = verification_window_start_position + mapping_start_position;
          uint16_t fragment_length = position - fragment_start_position + 1;
          mapq = GetMAPQ(0, 0, fragment_length, min_num_errors, num_best_mappings, second_min_num_errors, num_second_best_mappings);
          mapq |= 1;
          if (output_mapping_in_PAF_) {
            EmplaceBackMappingRecord(read_id, read_name, (uint16_t)read_length, barcode_key, fragment_start_position, fragment_length, mapq, &((*mappings_on_diff_ref_seqs)[rid]));
          } else {
            EmplaceBackMappingRecord(read_id, barcode_key, fragment_start_position, fragment_length, mapq, &((*mappings_on_diff_ref_seqs)[rid]));
          }
        } else {
          BandedTraceback(min_num_errors, reference.GetSequenceAt(rid) + verification_window_start_position, negative_read.data(), read_length, &mapping_start_position);
          uint32_t fragment_start_position = verification_window_start_position + mapping_start_position;
          uint16_t fragment_length = position - fragment_start_position + 1;
          if (output_mapping_in_PAF_) {
            EmplaceBackMappingRecord(read_id, read_name, (uint16_t)read_length, barcode_key, fragment_start_position, fragment_length, mapq, &((*mappings_on_diff_ref_seqs)[rid]));
          } else {
            EmplaceBackMappingRecord(read_id, barcode_key, fragment_start_position, fragment_length, mapq, &((*mappings_on_diff_ref_seqs)[rid]));
          }
        }
        (*num_best_mappings_reported)++;
        if (*num_best_mappings_reported == std::min(max_num_best_mappings_, num_best_mappings)) {
          break;
        }
      }
      (*best_mapping_index)++;
    }
  }
}

template <typename MappingRecord>
void Chromap<MappingRecord>::GenerateBestMappingsForSingleEndRead(int min_num_errors, int num_best_mappings, int second_min_num_errors, int num_second_best_mappings, const SequenceBatch &read_batch, uint32_t read_index, const SequenceBatch &reference, const SequenceBatch &barcode_batch, const std::vector<std::pair<int, uint64_t> > &positive_mappings, const std::vector<std::pair<int, uint64_t> > &negative_mappings, std::vector<std::vector<MappingRecord> > *mappings_on_diff_ref_seqs) {
  //uint8_t mapq = GetMAPQ(num_best_mappings, num_second_best_mappings);
  uint8_t mapq = 0;
  // we will use reservoir sampling 
  std::vector<int> best_mapping_indices(max_num_best_mappings_);
  std::iota(best_mapping_indices.begin(), best_mapping_indices.end(), 0);
  if (num_best_mappings > max_num_best_mappings_) {
    std::mt19937 generator(11);
    for (int i = max_num_best_mappings_; i < num_best_mappings; ++i) {
      std::uniform_int_distribution<int> distribution(0, i); // important: inclusive range
      int j = distribution(generator); 
      if (j < max_num_best_mappings_) {
        best_mapping_indices[j] = i;
      }
    }
    std::sort(best_mapping_indices.begin(), best_mapping_indices.end());
  }
  int best_mapping_index = 0;
  int num_best_mappings_reported = 0;
  ProcessBestMappingsForSingleEndRead(kPositive, mapq, min_num_errors, num_best_mappings, second_min_num_errors, num_second_best_mappings, read_batch, read_index, reference, barcode_batch, best_mapping_indices, positive_mappings, &best_mapping_index, &num_best_mappings_reported, mappings_on_diff_ref_seqs);
  if (num_best_mappings_reported != std::min(num_best_mappings, max_num_best_mappings_)) {
    ProcessBestMappingsForSingleEndRead(kNegative, mapq, min_num_errors, num_best_mappings, second_min_num_errors, num_second_best_mappings, read_batch, read_index, reference, barcode_batch, best_mapping_indices, negative_mappings, &best_mapping_index, &num_best_mappings_reported, mappings_on_diff_ref_seqs);
  }
}

template <typename MappingRecord>
void Chromap<MappingRecord>::ApplyTn5ShiftOnSingleEndMapping(uint32_t num_reference_sequences, std::vector<std::vector<MappingRecord> > *mappings) {
  uint64_t num_shifted_mappings = 0;
  for (auto &mappings_on_one_ref_seq : *mappings) {
    for(auto &mapping: mappings_on_one_ref_seq) {
      uint8_t strand = mapping.mapq & 1;
      if (strand == 1) {
        mapping.fragment_start_position += 4;
        mapping.fragment_length -= 4;
      } else {
        mapping.fragment_length -= 5;
      }
      ++num_shifted_mappings;
    }
  } 
  std::cerr << "# shifted mappings: " << num_shifted_mappings << ".\n";
}

template <typename MappingRecord>
uint32_t Chromap<MappingRecord>::LoadSingleEndReadsWithBarcodes(SequenceBatch *read_batch, SequenceBatch *barcode_batch) {
  double real_start_time = Chromap<>::GetRealTime();
  uint32_t num_loaded_reads = 0;
  while (num_loaded_reads < read_batch_size_) {
    bool no_more_read = read_batch->LoadOneSequenceAndSaveAt(num_loaded_reads);
    bool no_more_barcode = no_more_read;
    if (!is_bulk_data_) {
      no_more_barcode = barcode_batch->LoadOneSequenceAndSaveAt(num_loaded_reads);
    }
    if ((!no_more_read) && (!no_more_barcode)) {
      if (read_batch->GetSequenceLengthAt(num_loaded_reads) < (uint32_t)min_read_length_) {
        continue; // reads are too short, just drop.
      }
      //if (PairedEndReadWithBarcodeIsDuplicate(num_loaded_pairs, (*barcode_batch), (*read_batch1), (*read_batch2))) {
      //  num_duplicated_reads_ += 2;
      //  continue;
      //}
    } else if (no_more_read && no_more_barcode) {
      break;
    } else {
      Chromap<>::ExitWithMessage("Numbers of reads and barcodes don't match!");
    }
    ++num_loaded_reads;
  }
  if (num_loaded_reads > 0) {
    std::cerr << "Loaded " << num_loaded_reads << " reads in "<< Chromap<>::GetRealTime() - real_start_time << "s.\n";
  } else {
    std::cerr << "No more reads.\n";
  }
  return num_loaded_reads;
}

template <typename MappingRecord>
void Chromap<MappingRecord>::ConstructIndex() {
  // TODO(Haowen): check args for index construction
  // TODO(Haowen): Need a faster algorithm
  // Load all sequences in the reference into one batch
  SequenceBatch reference;
  reference.InitializeLoading(reference_file_path_);
  uint32_t num_sequences = reference.LoadAllSequences();
  Index index(kmer_size_, window_size_, num_threads_, index_file_path_);
  index.Construct(num_sequences, reference);
  index.Statistics(num_sequences, reference);
  index.Save();
  reference.FinalizeLoading();
}

template <typename MappingRecord>
void Chromap<MappingRecord>::MoveMappingsInBuffersToMappingContainer(uint32_t num_reference_sequences, std::vector<std::vector<std::vector<MappingRecord> > > *mappings_on_diff_ref_seqs_for_diff_threads_for_saving) {
  double real_start_time = Chromap<>::GetRealTime();
  for (int ti = 0; ti < num_threads_; ++ti) {
    for (uint32_t i = 0; i < num_reference_sequences; ++i) {
      mappings_on_diff_ref_seqs_[i].insert(mappings_on_diff_ref_seqs_[i].end(), std::make_move_iterator((*mappings_on_diff_ref_seqs_for_diff_threads_for_saving)[ti][i].begin()), std::make_move_iterator((*mappings_on_diff_ref_seqs_for_diff_threads_for_saving)[ti][i].end()));
      (*mappings_on_diff_ref_seqs_for_diff_threads_for_saving)[ti][i].clear();
    }
  }
  std::cerr << "Move mappings in " << Chromap<>::GetRealTime() - real_start_time << "s.\n";
}

template <typename MappingRecord>
void Chromap<MappingRecord>::SortOutputMappings(uint32_t num_reference_sequences, std::vector<std::vector<MappingRecord> > *mappings) {
  double real_dedupe_start_time = Chromap<>::GetRealTime();
  uint32_t num_mappings = 0;
  for (uint32_t ri = 0; ri < num_reference_sequences; ++ri) {
    std::sort((*mappings)[ri].begin(), (*mappings)[ri].end());
    num_mappings += (*mappings)[ri].size(); 
  }
  std::cerr << "Sorted " << num_mappings << " elements in " << Chromap<>::GetRealTime() - real_dedupe_start_time << "s.\n";
}

template <typename MappingRecord>
void Chromap<MappingRecord>::RemovePCRDuplicate(uint32_t num_reference_sequences) {
  uint32_t num_mappings = 0;
  double real_dedupe_start_time = Chromap<>::GetRealTime();
  for (uint32_t ri = 0; ri < num_reference_sequences; ++ri) {
    //double real_start_time = Chromap<>::GetRealTime();
    //radix_sort_with_barcode(mappings_on_diff_ref_seqs_[ri].data(), mappings_on_diff_ref_seqs_[ri].data() + mappings_on_diff_ref_seqs_[ri].size());
    std::sort(mappings_on_diff_ref_seqs_[ri].begin(), mappings_on_diff_ref_seqs_[ri].end());
    num_mappings += mappings_on_diff_ref_seqs_[ri].size(); 
    //std::cerr << "Sorted " << mappings_on_diff_ref_seqs_[ri].size() << " elements on " << ri << " in " << Chromap<>::GetRealTime() - real_start_time << "s.\n";
  }
  std::cerr << "Sorted " << num_mappings << " elements in " << Chromap<>::GetRealTime() - real_dedupe_start_time << "s.\n";
  num_mappings = 0;
  for (uint32_t ri = 0; ri < num_reference_sequences; ++ri) {
    if (mappings_on_diff_ref_seqs_[ri].size() != 0) {
      deduped_mappings_on_diff_ref_seqs_[ri].emplace_back(mappings_on_diff_ref_seqs_[ri].front());
      //std::vector<MappingRecord>::iterator last_it = mappings_on_diff_ref_seqs_[ri].begin();
      auto last_it = mappings_on_diff_ref_seqs_[ri].begin();
      //for (std::vector<MappingRecord>::iterator it = ++(mappings_on_diff_ref_seqs_[ri].begin()); it != mappings_on_diff_ref_seqs_[ri].end(); ++it) {
      for (auto it = ++(mappings_on_diff_ref_seqs_[ri].begin()); it != mappings_on_diff_ref_seqs_[ri].end(); ++it) {
        if (!((*it) == (*last_it))) {
          deduped_mappings_on_diff_ref_seqs_[ri].emplace_back((*it));
          last_it = it;
        }
      }
      std::vector<MappingRecord>().swap(mappings_on_diff_ref_seqs_[ri]);
      num_mappings += deduped_mappings_on_diff_ref_seqs_[ri].size();
    }
  }
  std::cerr << num_mappings << " mappings left after dedupe in " << Chromap<>::GetRealTime() - real_dedupe_start_time << "s.\n";
}

template <typename MappingRecord>
void Chromap<MappingRecord>::BuildAugmentedTree(uint32_t ref_id) {
  //std::sort(mappings.begin(), mappings.end(), IntervalLess());
  int max_level = 0;
  size_t i, last_i = 0; // last_i points to the rightmost node in the tree
  uint32_t last = 0; // last is the max value at node last_i
  int k;
  std::vector<MappingRecord> &mappings = allocated_mappings_on_diff_ref_seqs_[ref_id];
  std::vector<uint32_t> &extras = tree_extras_on_diff_ref_seqs_[ref_id];
  if (mappings.size() == 0) {
    max_level = -1;
  }
  for (i = 0; i < mappings.size(); i += 2) { 
    last_i = i; 
    //last = mappings[i].max = mappings[i].en; // leaves (i.e. at level 0)
    last = extras[i] = mappings[i].fragment_start_position + mappings[i].fragment_length; // leaves (i.e. at level 0)
  }
  for (k = 1; 1LL<<k <= (int64_t)mappings.size(); ++k) { // process internal nodes in the bottom-up order
    size_t x = 1LL<<(k-1);
    size_t i0 = (x<<1) - 1;
    size_t step = x<<2; // i0 is the first node
    for (i = i0; i < mappings.size(); i += step) { // traverse all nodes at level k
      uint32_t el = extras[i - x];                          // max value of the left child
      uint32_t er = i + x < mappings.size()? extras[i + x] : last; // of the right child
      uint32_t e = mappings[i].fragment_start_position + mappings[i].fragment_length;
      e = e > el? e : el;
      e = e > er? e : er;
      extras[i] = e; // set the max value for node i
    }
    last_i = last_i>>k&1? last_i - x : last_i + x; // last_i now points to the parent of the original last_i
    if (last_i < mappings.size() && extras[last_i] > last) // update last accordingly
      last = extras[last_i];
  }
  max_level = k - 1;
  tree_info_on_diff_ref_seqs_.emplace_back(max_level, mappings.size());
}

template <typename MappingRecord>
uint32_t Chromap<MappingRecord>::GetNumOverlappedMappings(uint32_t ref_id, const MappingRecord &mapping) {
  int t = 0;
  StackCell stack[64];
  //out.clear();
  int num_overlapped_mappings = 0;
  int max_level = tree_info_on_diff_ref_seqs_[ref_id].first;
  uint32_t num_tree_nodes = tree_info_on_diff_ref_seqs_[ref_id].second;
  std::vector<MappingRecord> &mappings = allocated_mappings_on_diff_ref_seqs_[ref_id];
  std::vector<uint32_t> &extras = tree_extras_on_diff_ref_seqs_[ref_id];
  //uint32_t interval_start = mapping.fragment_start_position; 
  uint32_t interval_start = mapping.fragment_start_position > (uint32_t)multi_mapping_allocation_distance_ ? mapping.fragment_start_position - multi_mapping_allocation_distance_ : 0;
  uint32_t interval_end = mapping.fragment_start_position + mapping.fragment_length + (uint32_t)multi_mapping_allocation_distance_;
  stack[t++] = StackCell(max_level, (1LL<<max_level) - 1, 0); // push the root; this is a top down traversal
  while (t) { // the following guarantees that numbers in out[] are always sorted
    StackCell z = stack[--t];
    if (z.k <= 3) { // we are in a small subtree; traverse every node in this subtree
      size_t i, i0 = z.x >> z.k << z.k, i1 = i0 + (1LL<<(z.k+1)) - 1;
      if (i1 >= num_tree_nodes) {
        i1 = num_tree_nodes;
      }
      for (i = i0; i < i1 && mappings[i].fragment_start_position < interval_end; ++i) {
        if (interval_start < mappings[i].fragment_start_position + mappings[i].fragment_length) { // if overlap, append to out[]
          //out.push_back(i);
          ++num_overlapped_mappings;
        }
      }
    } else if (z.w == 0) { // if left child not processed
      size_t y = z.x - (1LL<<(z.k-1)); // the left child of z.x; NB: y may be out of range (i.e. y>=a.size())
      stack[t++] = StackCell(z.k, z.x, 1); // re-add node z.x, but mark the left child having been processed
      if (y >= num_tree_nodes || extras[y] > interval_start) // push the left child if y is out of range or may overlap with the query
        stack[t++] = StackCell(z.k - 1, y, 0);
    } else if (z.x < num_tree_nodes && mappings[z.x].fragment_start_position < interval_end) { // need to push the right child
      if (interval_start < mappings[z.x].fragment_start_position + mappings[z.x].fragment_length) {
        //out.push_back(z.x); // test if z.x overlaps the query; if yes, append to out[]
        ++num_overlapped_mappings;
      }
      stack[t++] = StackCell(z.k - 1, z.x + (1LL<<(z.k-1)), 0); // push the right child
    }
  }
  return num_overlapped_mappings;
}

template <typename MappingRecord>
void Chromap<MappingRecord>::AllocateMultiMappings(uint32_t num_reference_sequences) {
  double real_start_time = Chromap<>::GetRealTime();
  std::vector<std::vector<MappingRecord> > &mappings = remove_pcr_duplicates_ ? deduped_mappings_on_diff_ref_seqs_ : mappings_on_diff_ref_seqs_;
  multi_mappings_.reserve((num_mapped_reads_ - num_uniquely_mapped_reads_) / 2);
  allocated_mappings_on_diff_ref_seqs_.reserve(num_reference_sequences);
  tree_extras_on_diff_ref_seqs_.reserve(num_reference_sequences);
  tree_info_on_diff_ref_seqs_.reserve(num_reference_sequences);
  // two passes, one for memory pre-allocation, another to move the mappings.
  for (uint32_t ri = 0; ri < num_reference_sequences; ++ri) {
    allocated_mappings_on_diff_ref_seqs_.emplace_back(std::vector<MappingRecord>());
    tree_extras_on_diff_ref_seqs_.emplace_back(std::vector<uint32_t>());
    uint32_t num_uni_mappings = 0;
    uint32_t num_multi_mappings = 0;
    for (uint32_t mi = 0; mi < mappings[ri].size(); ++mi) {
      MappingRecord &mapping = mappings[ri][mi];
      if ((mapping.mapq >> 1) < 30) { // we have to ensure that the mapq is lower than 30 if and only if it is a multi-read.
        ++num_multi_mappings;
      } else {
        ++num_uni_mappings;
      }
    }
    allocated_mappings_on_diff_ref_seqs_[ri].reserve(num_uni_mappings);
    tree_extras_on_diff_ref_seqs_[ri].reserve(num_uni_mappings);
    for (uint32_t mi = 0; mi < mappings[ri].size(); ++mi) {
      MappingRecord &mapping = mappings[ri][mi];
      if ((mapping.mapq >> 1) < 30) { // we have to ensure that the mapq is lower than 30 if and only if it is a multi-read.
        multi_mappings_.emplace_back(ri, mapping);
      } else {
        allocated_mappings_on_diff_ref_seqs_[ri].emplace_back(mapping);
        tree_extras_on_diff_ref_seqs_[ri].emplace_back(0);
      }
    }
    std::vector<MappingRecord>().swap(mappings[ri]);
    BuildAugmentedTree(ri);
  }
  std::cerr << "Got all " << multi_mappings_.size() << " multi-mappings!\n";
  std::stable_sort(multi_mappings_.begin(), multi_mappings_.end(), ReadIdLess<MappingRecord>);
  std::vector<uint32_t> weights;
  weights.reserve(max_num_best_mappings_);
  uint32_t sum_weight = 0;
  assert(multi_mappings_.size() > 0);
  uint32_t previous_read_id = multi_mappings_[0].second.read_id;
  uint32_t start_mapping_index = 0;
  // add a fake mapping at the end and make sure its id is different from the last one
  assert(multi_mappings_.size() != UINT32_MAX);
  std::pair<uint32_t, MappingRecord> foo_mapping = multi_mappings_.back();
  foo_mapping.second.read_id = UINT32_MAX;
  multi_mappings_.emplace_back(foo_mapping);
  std::mt19937 generator(multi_mapping_allocation_seed_);
  uint32_t current_read_id;//, reference_id, mapping_index;
  //uint32_t allocated_read_id, allocated_reference_id, allocated_mapping_index;
  uint32_t num_allocated_multi_mappings = 0;
  uint32_t num_multi_mappings_without_overlapping_unique_mappings = 0;
  for (uint32_t mi = 0; mi < multi_mappings_.size(); ++mi) {
    std::pair<uint32_t, MappingRecord> &current_multi_mapping = multi_mappings_[mi];//mappings[reference_id][mapping_index];
    current_read_id = current_multi_mapping.second.read_id;
    uint32_t num_overlaps = GetNumOverlappedMappings(current_multi_mapping.first, current_multi_mapping.second);
    //std::cerr << mi << " " << current_read_id << " " << previous_read_id << " " << reference_id << " " << mapping_index << " " << interval_start << " " << num_overlaps << " " << sum_weight << "\n";
    if (current_read_id == previous_read_id) {
      weights.emplace_back(num_overlaps);
      sum_weight += num_overlaps;
    } else {
      // deal with the previous one.
      if (sum_weight == 0) {
        ++num_multi_mappings_without_overlapping_unique_mappings;
        //assert(weights.size() > 1); // After PCR dedupe, some multi-reads may become uni-reads. For now, we just assign it to that unique mapping positions.
        //std::fill(weights.begin(), weights.end(), 1); // We drop the multi-mappings that have no overlap with uni-mappings.
      } else {
        std::discrete_distribution<uint32_t> distribution(weights.begin(), weights.end());
        uint32_t randomly_assigned_mapping_index = distribution(generator);
        allocated_mappings_on_diff_ref_seqs_[multi_mappings_[start_mapping_index + randomly_assigned_mapping_index].first].emplace_back(multi_mappings_[start_mapping_index + randomly_assigned_mapping_index].second);
        ++num_allocated_multi_mappings;
      }
      // update current
      weights.clear();
      weights.emplace_back(num_overlaps);
      sum_weight = num_overlaps;
      start_mapping_index = mi;
      previous_read_id = current_read_id;
    }
  }
  std::cerr << "Allocated " << num_allocated_multi_mappings << " multi-mappings in "<< Chromap<>::GetRealTime() - real_start_time << "s.\n";
  std::cerr << "# multi-mappings that have no uni-mapping overlaps: " << num_multi_mappings_without_overlapping_unique_mappings << ".\n";
}

template <typename MappingRecord>
void Chromap<MappingRecord>::VerifyCandidatesOnOneDirection(Direction candidate_direction, const SequenceBatch &read_batch, uint32_t read_index, const SequenceBatch &reference, const std::vector<uint64_t> &candidates, std::vector<std::pair<int, uint64_t> > *mappings, int *min_num_errors, int *num_best_mappings, int *second_min_num_errors, int *num_second_best_mappings) {
  const char *read = read_batch.GetSequenceAt(read_index);
  uint32_t read_length = read_batch.GetSequenceLengthAt(read_index);
  const std::string &negative_read = read_batch.GetNegativeSequenceAt(read_index); 

  for (uint32_t ci = 0; ci < candidates.size(); ++ci) {
    uint32_t rid = candidates[ci] >> 32;
    uint32_t position = candidates[ci];
    if (candidate_direction == kNegative) {
      position = position - read_length + 1;
    }
    if (position < (uint32_t)error_threshold_ || position >= reference.GetSequenceLengthAt(rid) || position + read_length + error_threshold_ >= reference.GetSequenceLengthAt(rid)) {
      continue;
    }
    int mapping_end_position;
    int num_errors;
    if (candidate_direction == kPositive) {
      num_errors = BandedAlignPatternToText(reference.GetSequenceAt(rid) + position - error_threshold_, read, read_length, &mapping_end_position);
    } else {
      num_errors = BandedAlignPatternToText(reference.GetSequenceAt(rid) + position - error_threshold_, negative_read.data(), read_length, &mapping_end_position);
    }
    if (num_errors <= error_threshold_) {
      if (num_errors < *min_num_errors) {
        *second_min_num_errors = *min_num_errors;
        *num_second_best_mappings = *num_best_mappings;
        *min_num_errors = num_errors;
        *num_best_mappings = 1;
      } else if (num_errors == *min_num_errors) {
        (*num_best_mappings)++;
      } else if (num_errors == *second_min_num_errors) {
        (*num_second_best_mappings)++;
      }
      if (candidate_direction == kPositive) {
        mappings->emplace_back(num_errors, candidates[ci] - error_threshold_ + mapping_end_position);
      } else {
        mappings->emplace_back(num_errors, candidates[ci] - read_length + 1 - error_threshold_ + mapping_end_position); 
      }
    }
  }
}

template <typename MappingRecord>
void Chromap<MappingRecord>::VerifyCandidates(const SequenceBatch &read_batch, uint32_t read_index, const SequenceBatch &reference, const std::vector<uint64_t> &positive_candidates, const std::vector<uint64_t> &negative_candidates, std::vector<std::pair<int, uint64_t> > *positive_mappings, std::vector<std::pair<int, uint64_t> > *negative_mappings, int *min_num_errors, int *num_best_mappings, int *second_min_num_errors, int *num_second_best_mappings) {
  *min_num_errors = error_threshold_ + 1;
  *num_best_mappings = 0;
  *second_min_num_errors = error_threshold_ + 1;
  *num_second_best_mappings = 0;
  VerifyCandidatesOnOneDirection(kPositive, read_batch, read_index, reference, positive_candidates, positive_mappings, min_num_errors, num_best_mappings, second_min_num_errors, num_second_best_mappings);
  VerifyCandidatesOnOneDirection(kNegative, read_batch, read_index, reference, negative_candidates, negative_mappings, min_num_errors, num_best_mappings, second_min_num_errors, num_second_best_mappings);
}

template <typename MappingRecord>
int Chromap<MappingRecord>::BandedAlignPatternToText(const char *pattern, const char *text, const int read_length, int *mapping_end_position) {
  uint32_t Peq[5] = {0, 0, 0, 0, 0};
  for (int i = 0; i < 2 * error_threshold_; i++) {
    uint8_t base = SequenceBatch::CharToUint8(pattern[i]);
    Peq[base] = Peq[base] | (1 << i);
  }
  uint32_t highest_bit_in_band_mask = 1 << (2 * error_threshold_);
  uint32_t lowest_bit_in_band_mask = 1;
  uint32_t VP = 0;
  uint32_t VN = 0;
  uint32_t X = 0;
  uint32_t D0 = 0;
  uint32_t HN = 0;
  uint32_t HP = 0;
  int num_errors_at_band_start_position = 0;
  for (int i = 0; i < read_length; i++) {
    uint8_t pattern_base = SequenceBatch::CharToUint8(pattern[i + 2 * error_threshold_]);
    Peq[pattern_base] = Peq[pattern_base] | highest_bit_in_band_mask;
    X = Peq[SequenceBatch::CharToUint8(text[i])] | VN;
    D0 = ((VP + (X & VP)) ^ VP) | X;
    HN = VP & D0;
    HP = VN | ~(VP | D0);
    X = D0 >> 1;
    VN = X & HP;
    VP = HN | ~(X | HP);
    num_errors_at_band_start_position += 1 - (D0 & lowest_bit_in_band_mask);
    if (num_errors_at_band_start_position > 3 * error_threshold_) {
      return error_threshold_ + 1;
    }
    for (int ai = 0; ai < 5; ai++) {
      Peq[ai] >>= 1;
    }
  }
  int band_start_position = read_length - 1;
  int min_num_errors = num_errors_at_band_start_position;
  *mapping_end_position = band_start_position;
  for (int i = 0; i < 2 * error_threshold_; i++) {
    num_errors_at_band_start_position = num_errors_at_band_start_position + ((VP >> i) & (uint32_t) 1);
    num_errors_at_band_start_position = num_errors_at_band_start_position - ((VN >> i) & (uint32_t) 1);
    if (num_errors_at_band_start_position < min_num_errors) {
      min_num_errors = num_errors_at_band_start_position;
      *mapping_end_position = band_start_position + 1 + i;
    }
  }
  return min_num_errors;
}

template <typename MappingRecord>
void Chromap<MappingRecord>::BandedTraceback(int min_num_errors, const char *pattern, const char *text, const int read_length, int *mapping_start_position) {
  // fisrt calculate the hamming distance and see whether it's equal to # errors
  if (min_num_errors == 0) {
    *mapping_start_position = error_threshold_;
    return;
  } 
  int error_count = 0;
  for (int i = 0; i < read_length; ++i) {
    if (pattern[i + error_threshold_] != text[i]) {
      ++error_count;
    } 
  }
  if (error_count == min_num_errors) {
    *mapping_start_position = error_threshold_;
    return;
  }
  // if not then there are gaps so that we have to traceback with edit distance.
  uint32_t Peq[5] = {0, 0, 0, 0, 0};
  for (int i = 0; i < 2 * error_threshold_; i++) {
    uint8_t base = SequenceBatch::CharToUint8(pattern[read_length - 1 + 2 * error_threshold_ - i]);
    Peq[base] = Peq[base] | (1 << i);
  }
  uint32_t highest_bit_in_band_mask = 1 << (2 * error_threshold_);
  uint32_t lowest_bit_in_band_mask = 1;
  uint32_t VP = 0;
  uint32_t VN = 0;
  uint32_t X = 0;
  uint32_t D0 = 0;
  uint32_t HN = 0;
  uint32_t HP = 0;
  int num_errors_at_band_start_position = 0;
  for (int i = 0; i < read_length; i++) {
    uint8_t pattern_base = SequenceBatch::CharToUint8(pattern[read_length - 1 - i]);
    Peq[pattern_base] = Peq[pattern_base] | highest_bit_in_band_mask;
    X = Peq[SequenceBatch::CharToUint8(text[read_length - 1 - i])] | VN;
    D0 = ((VP + (X & VP)) ^ VP) | X;
    HN = VP & D0;
    HP = VN | ~(VP | D0);
    X = D0 >> 1;
    VN = X & HP;
    VP = HN | ~(X | HP);
    num_errors_at_band_start_position += 1 - (D0 & lowest_bit_in_band_mask);
    for (int ai = 0; ai < 5; ai++) {
      Peq[ai] >>= 1;
    }
  }
  *mapping_start_position = 2 * error_threshold_;
  for (int i = 0; i < 2 * error_threshold_; i++) {
    num_errors_at_band_start_position = num_errors_at_band_start_position + ((VP >> i) & (uint32_t) 1);
    num_errors_at_band_start_position = num_errors_at_band_start_position - ((VN >> i) & (uint32_t) 1);
    if (num_errors_at_band_start_position == min_num_errors) {
      *mapping_start_position = 2 * error_threshold_ - (1 + i);
    }
  }
}

template <typename MappingRecord>
void Chromap<MappingRecord>::OutputMappingStatistics() {
  std::cerr << "Number of reads: " << num_reads_ << ".\n";
  //std::cerr << "Number of duplicated reads: " << num_duplicated_reads_ << ".\n";
  std::cerr << "Number of mapped reads: " << num_mapped_reads_ << ".\n";
  std::cerr << "Number of uniquely mapped reads: " << num_uniquely_mapped_reads_ << ".\n";
  std::cerr << "Number of reads have multi-mappings: " << num_mapped_reads_ - num_uniquely_mapped_reads_ << ".\n";
  std::cerr << "Number of candidates: " << num_candidates_ << ".\n";
  std::cerr << "Number of mappings: " << num_mappings_ << ".\n";
  std::cerr << "Number of uni-mappings: " << num_uniquely_mapped_reads_ << ".\n";
  std::cerr << "Number of multi-mappings: " << num_mappings_ - num_uniquely_mapped_reads_ << ".\n";
}

template <typename MappingRecord>
void Chromap<MappingRecord>::OutputMappingStatistics(uint32_t num_reference_sequences, const std::vector<std::vector<MappingRecord> > &uni_mappings, const std::vector<std::vector<MappingRecord> > &multi_mappings) {
  uint64_t num_uni_mappings = 0;
  uint64_t num_multi_mappings = 0;
  for (auto &uni_mappings_on_one_ref_seq : uni_mappings) {
    for(auto &uni_mapping: uni_mappings_on_one_ref_seq) {
      if ((uni_mapping.mapq >> 1) >= 30) {
        ++num_uni_mappings;
      }
    }
  } 
  for (auto &multi_mappings_on_one_ref_seq : multi_mappings) {
    for(auto &multi_mapping: multi_mappings_on_one_ref_seq) {
      if ((multi_mapping.mapq >> 1) < 30) {
        ++num_multi_mappings;
      }
    }
  } 
  std::cerr << "# uni-mappings: " << num_uni_mappings << ", # multi-mappings: " << num_multi_mappings << ", total: " << num_uni_mappings + num_multi_mappings << ".\n";
}

template <typename MappingRecord>
uint8_t Chromap<MappingRecord>::GetMAPQ(int num_positive_candidates, int num_negative_candidates, uint16_t alignment_length, int min_num_errors, int num_best_mappings, int second_min_num_errors, int num_second_best_mappings) {
  double alignment_identity = 1 - (double)min_num_errors / alignment_length;
  int mapq = 0;
  if (num_best_mappings > 1) {
    mapq = -4.343 * log(1 - 1.0 / num_best_mappings);
  } else {
    if (second_min_num_errors > error_threshold_) {
        second_min_num_errors = 2 * error_threshold_ + 1;
    }
    mapq = (int)(60 * (1. - (double)min_num_errors / second_min_num_errors) + .499);
    double tmp = alignment_identity * alignment_identity;
    tmp = tmp * tmp;
    tmp = tmp * tmp;
    mapq = alignment_identity < 0.98? (int)(mapq * tmp + .499) : mapq;
  }
  if (num_second_best_mappings > 0) {
    mapq -= (int)(4.343 * log(num_second_best_mappings + 1) + 0.499);
  }
  if (num_positive_candidates > 1 || num_negative_candidates > 1) {
    mapq -= (int)(4.343 * log(num_positive_candidates + num_negative_candidates) + 0.499);
  }
  if (mapq > 60) {
    mapq = 60;
  }
  if (mapq < 0) {
    mapq = 0;
  }
  //uint8_t mapq = 0;
  //if (num_best_mappings == 1 && num_second_best_mappings == 0 && min_num_errors < 2) {
  //  mapq = 60;
  //} else if (num_best_mappings == 1 && num_second_best_mappings > 0 && (second_min_num_errors - min_num_errors) >= 2) {
  //  mapq = 30;
  //}
  //else if (num_best_mappings == 2) {
  //  mapq = 15;
  //} else if (num_best_mappings == 3) {
  //  mapq = 10;
  //} else if (num_best_mappings == 4) {
  //  mapq = 5;
  //} else if (num_best_mappings == 5) {
  //  mapq = 1;
  //}
  mapq <<= 1;
  return (uint8_t)mapq;
}
} // namespace chromap

int main(int argc, char *argv[]) {
  cxxopts::Options options("chromap", "A short read mapper for chromatin biology");
  options.add_options("Indexing")
    ("i,build-index", "Build index")
    ("k,kmer", "Kmer length [17]", cxxopts::value<int>(), "INT")
    ("w,window", "Window size [5]", cxxopts::value<int>(), "INT");
  options.add_options("Mapping")
    ("m,map", "Map reads")
    ("e,error-threshold", "Max # errors allowed to map a read [3]", cxxopts::value<int>(), "INT")
    ("A,match-score", "Match score [1]", cxxopts::value<int>(), "INT")
    ("B,mismatch-penalty", "Mismatch penalty [4]", cxxopts::value<int>(), "INT")
    ("O,gap-open-penalties", "Gap open penalty [6,6]", cxxopts::value<std::vector<int>>(), "INT[,INT]")
    ("E,gap-extension-penalties", "Gap extension penalty [1,1]", cxxopts::value<std::vector<int>>(), "INT[,INT]")
    ("s,min-num-seeds", "Min # seeds to try to map a read [2]", cxxopts::value<int>(), "INT")
    ("f,max-seed-frequencies", "Max seed frequencies for a seed to be selected [1000,5000]", cxxopts::value<std::vector<int>>(), "INT[,INT]")
    ("n,max-num-best-mappings", "Only report n best mappings [10]", cxxopts::value<int>(), "INT")
    ("l,max-insert-size", "Max insert size, only for paired-end read mapping [400]", cxxopts::value<int>(), "INT")
    ("min-read-length", "Min read length [30]", cxxopts::value<int>(), "INT")
    ("multi-mapping-allocation-distance", "Uni-mappings within this distance from any end of multi-mappings are used for allocation [0]", cxxopts::value<int>(), "INT")
    ("multi-mapping-allocation-seed", "Seed for random number generator in multi-mapping allocation [11]", cxxopts::value<int>(), "INT")
    ("drop-repetitive-reads", "Drop reads with too many best mappings [500000]", cxxopts::value<int>(), "INT")
    ("trim-adapters", "Try to trim adapters on 3'")
    ("remove-pcr-duplicates", "Remove PCR duplicates")
    ("allocate-multi-mappings", "Allocate multi-mappings")
    ("unique-mappings", "Only output unique mappings")
    ("Tn5-shift", "Perform Tn5 shift")
    ("t,num-threads", "# threads for mapping [1]", cxxopts::value<int>(), "INT");
  options.add_options("Input")
    ("r,ref", "Reference file", cxxopts::value<std::string>(), "FILE")
    ("x,index", "Index file", cxxopts::value<std::string>(), "FILE")
    ("1,read1", "Single-end read file or paired-end read file 1", cxxopts::value<std::string>(), "FILE")
    ("2,read2", "Paired-end read file 2", cxxopts::value<std::string>(), "FILE")
    ("b,barcode", "Cell barcode file", cxxopts::value<std::string>(), "FILE");
  options.add_options("Output")
    ("o,output", "Output file", cxxopts::value<std::string>(), "FILE")
    ("BED", "Output mappings in BED/BEDPE format")
    ("TagAlign", "Output mappings in TagAlign/PairedTagAlign format")
    ("PAF", "Output mappings in PAF format");
  options.add_options()
    ("h,help", "Print help");

  auto result = options.parse(argc, argv);
  // Optional parameters
  int kmer_size = 17;
  if (result.count("k")) {
    kmer_size = result["kmer"].as<int>();
  }
  int window_size = 5;
  if (result.count("w")) {
    window_size = result["window"].as<int>();
  }
  int error_threshold = 3;
  if (result.count("e")) {
    error_threshold = result["error-threshold"].as<int>();
  }
  int match_score = 1;
  if (result.count("A")) {
    match_score = result["match-score"].as<int>();
  }
  int mismatch_penalty = 4;
  if (result.count("B")) {
    mismatch_penalty = result["mismatch-penalty"].as<int>();
  }
  std::vector<int> gap_open_penalties = {6, 6};
  if (result.count("O")) {
    gap_open_penalties = result["gap-open-penalties"].as<std::vector<int>>();
  }
  std::vector<int> gap_extension_penalties = {1, 1};
  if (result.count("E")) {
    gap_extension_penalties = result["gap-extension-penalties"].as<std::vector<int>>();
  }
  int min_num_seeds_required_for_mapping = 2;
  if (result.count("s")) {
    min_num_seeds_required_for_mapping = result["min-num-seeds"].as<int>();
  }
  std::vector<int> max_seed_frequencies = {1000, 5000};
  if (result.count("f")) {
    max_seed_frequencies = result["max-seed-frequencies"].as<std::vector<int>>();
  } 
  int max_num_best_mappings = 10; 
  if (result.count("n")) {
    max_num_best_mappings = result["max-num-best-mappings"].as<int>();
  } 
  int max_insert_size = 400;
  if (result.count("l")) {
    max_insert_size = result["max-insert-size"].as<int>();
  } 
  int num_threads = 1;
  if (result.count("t")) {
    num_threads = result["num-threads"].as<int>();
  } 
  int min_read_length = 30;
  if (result.count("min-read-length")) {
    min_read_length = result["min-read-length"].as<int>();
  }
  int multi_mapping_allocation_distance = 0;
  if (result.count("multi-mapping-allocation-distance")) {
    multi_mapping_allocation_distance = result["multi-mapping-allocation-distance"].as<int>();
  }
  int multi_mapping_allocation_seed = 11;
  if (result.count("multi-mapping-allocation-seed")) {
    multi_mapping_allocation_seed = result["multi-mapping-allocation-seed"].as<int>();
  }
  int drop_repetitive_reads = 500000;
  if (result.count("drop-repetitive-reads")) {
    drop_repetitive_reads = result["drop-repetitive-reads"].as<int>();
  }
  bool trim_adapters = false;
  if (result.count("trim-adapters")) {
    trim_adapters = true;
  }
  bool remove_pcr_duplicates = false;
  if (result.count("remove-pcr-duplicates")) {
    remove_pcr_duplicates = true;
  }
  bool allocate_multi_mappings = false;
  if (result.count("allocate-multi-mappings")) {
    allocate_multi_mappings = true;
  }
  bool only_output_unique_mappings = false;
  if (result.count("unique-mappings")) {
    only_output_unique_mappings = true;
  }
  bool Tn5_shift = false;
  if (result.count("Tn5-shift")) {
    Tn5_shift = true;
  }
  bool output_mapping_in_BED = false;
  if (result.count("BED")) {
    output_mapping_in_BED = true;
  }
  bool output_mapping_in_TagAlign = false;
  if (result.count("TagAlign")) {
    output_mapping_in_TagAlign = true;
  }
  bool output_mapping_in_PAF = false;
  if (result.count("PAF")) {
    output_mapping_in_PAF = true;
  }

  if (result.count("i")) {
    std::string reference_file_path;
    if (result.count("r")) {
      reference_file_path = result["ref"].as<std::string>();
    } else {
      chromap::Chromap<>::ExitWithMessage("No reference specified!");
    }
    std::string output_file_path;
    if (result.count("o")) {
      output_file_path = result["output"].as<std::string>();
    } else {
      chromap::Chromap<>::ExitWithMessage("No output file specified!");
    }
    std::cerr << "Build index for the reference.\n";
    std::cerr << "Kmer length: " << kmer_size << ", window size: " << window_size << "\n";
    std::cerr << "Reference file: " << reference_file_path << "\n";
    std::cerr << "Output file: " << output_file_path << "\n";
    chromap::Chromap<> chromap_for_indexing(kmer_size, window_size, num_threads, reference_file_path, output_file_path);
    chromap_for_indexing.ConstructIndex();
  } else if (result.count("m")) {
    std::cerr << "Map reads.\n";
    std::string reference_file_path;
    if (result.count("r")) {
      reference_file_path = result["ref"].as<std::string>();
    } else {
      chromap::Chromap<>::ExitWithMessage("No reference specified!");
    }
    std::string output_file_path;
    if (result.count("o")) {
      output_file_path = result["output"].as<std::string>();
    } else {
      chromap::Chromap<>::ExitWithMessage("No output file specified!");
    }
    std::string index_file_path;
    if (result.count("x")) {
      index_file_path = result["index"].as<std::string>();
    } else {
      chromap::Chromap<>::ExitWithMessage("No index file specified!");
    }
    std::string read_file1_path;
    if (result.count("1")) {
      read_file1_path = result["read1"].as<std::string>();
    } else {
      chromap::Chromap<>::ExitWithMessage("No read file specified!");
    }
    std::string read_file2_path;
    if (result.count("2")) {
      read_file2_path = result["read2"].as<std::string>();
    }
    std::string barcode_file_path;
    bool is_bulk_data = true;
    if (result.count("b")) {
      is_bulk_data = false;
      barcode_file_path = result["barcode"].as<std::string>();
    }
    std::cerr << "error threshold: " << error_threshold << ", match score: " << match_score << ", mismatch_penalty: " << mismatch_penalty << ", gap open penalties for deletions and insertions: " << gap_open_penalties[0] << "," << gap_open_penalties[1] << ", gap extension penalties for deletions and insertions: " << gap_extension_penalties[0] << "," << gap_extension_penalties[1] << ", min-num-seeds: " << min_num_seeds_required_for_mapping << ", max-seed-frequency: " << max_seed_frequencies[0] << "," << max_seed_frequencies[1] << ", max-num-best-mappings: " << max_num_best_mappings << ", max-insert-size: " << max_insert_size << ", min-read-length: " << min_read_length << ", multi-mapping-allocation-distance: " << multi_mapping_allocation_distance << ", multi-mapping-allocation-seed: " << multi_mapping_allocation_seed << ", drop-repetitive-reads: " << drop_repetitive_reads << "\n";
    std::cerr << "Number of threads: " << num_threads << "\n";
    if (is_bulk_data) {
      std::cerr << "Analyze bulk data.\n";
    } else {
      std::cerr << "Analyze single-cell data.\n";
    }
    if (trim_adapters) {
      std::cerr << "Will try to remove adapters on 3'.\n";
    } else {
      std::cerr << "Won't try to remove adapters on 3'.\n";
    }
    if (remove_pcr_duplicates) {
      std::cerr << "Will remove PCR duplicates after mapping.\n";
    } else {
      std::cerr << "Won't remove PCR duplicates after mapping.\n";
    }
    if (allocate_multi_mappings) {
      std::cerr << "Will allocate multi-mappings after mapping.\n";
    } else {
      std::cerr << "Won't allocate multi-mappings after mapping.\n";
    }
    if (only_output_unique_mappings) {
      std::cerr << "Only output unique mappings after mapping.\n";
    } 
    if (allocate_multi_mappings && only_output_unique_mappings) {
      std::cerr << "WARNING: you want to output unique mappings only but you ask to allocate multi-mappings! In this case, it won't allocate multi-mappings and will only output unique mappings.\n";
      allocate_multi_mappings = false;
    }
    if (max_num_best_mappings > drop_repetitive_reads) {
      std::cerr << "WARNING: you want to drop mapped reads with more than " << drop_repetitive_reads << " mappings. But you want to output top " << max_num_best_mappings << " best mappings. In this case, only reads with <=" << drop_repetitive_reads << " best mappings will be output.\n";
      max_num_best_mappings = drop_repetitive_reads;
    }
    if (Tn5_shift) {
      std::cerr << "Perform Tn5 shift.\n";
    }
    if (output_mapping_in_BED) {
      std::cerr << "Output mappings in BED/BEDPE format.\n";
    } else if (output_mapping_in_TagAlign) {
      std::cerr << "Output mappings in TagAlign/PairedTagAlign format.\n";
    } else if (output_mapping_in_PAF) {
      std::cerr << "Output mappings in PAF format.\n";
    } else {
      chromap::Chromap<>::ExitWithMessage("No output format specified!");
    }
    std::cerr << "Reference file: " << reference_file_path << "\n";
    std::cerr << "Index file: " << index_file_path << "\n";
    std::cerr << "Read file 1: " << read_file1_path << "\n";
    if (result.count("2") != 0) {
      std::cerr << "Read file 2: " << read_file2_path << "\n";
    }
    if (result.count("b") != 0) {
      std::cerr << "Cell barcode file: " << barcode_file_path << "\n";
    }
    std::cerr << "Output file: " << output_file_path << "\n";
    if (result.count("2") == 0) {
      if (output_mapping_in_PAF) {
        chromap::Chromap<chromap::PAFMapping> chromap_for_mapping(error_threshold, match_score, mismatch_penalty, gap_open_penalties, gap_extension_penalties, min_num_seeds_required_for_mapping, max_seed_frequencies, max_num_best_mappings, max_insert_size, num_threads, min_read_length, multi_mapping_allocation_distance, multi_mapping_allocation_seed, drop_repetitive_reads, trim_adapters, remove_pcr_duplicates, is_bulk_data, allocate_multi_mappings, only_output_unique_mappings, Tn5_shift, output_mapping_in_BED, output_mapping_in_TagAlign, output_mapping_in_PAF, reference_file_path, index_file_path, read_file1_path, read_file2_path, barcode_file_path, output_file_path);
        chromap_for_mapping.MapSingleEndReads();
      } else {
        if (result.count("b") != 0) {
          chromap::Chromap<chromap::MappingWithBarcode> chromap_for_mapping(error_threshold, match_score, mismatch_penalty, gap_open_penalties, gap_extension_penalties, min_num_seeds_required_for_mapping, max_seed_frequencies, max_num_best_mappings, max_insert_size, num_threads, min_read_length, multi_mapping_allocation_distance, multi_mapping_allocation_seed, drop_repetitive_reads, trim_adapters, remove_pcr_duplicates, is_bulk_data, allocate_multi_mappings, only_output_unique_mappings, Tn5_shift, output_mapping_in_BED, output_mapping_in_TagAlign, output_mapping_in_PAF, reference_file_path, index_file_path, read_file1_path, read_file2_path, barcode_file_path, output_file_path);
          chromap_for_mapping.MapSingleEndReads();
        } else {
          chromap::Chromap<chromap::MappingWithoutBarcode> chromap_for_mapping(error_threshold, match_score, mismatch_penalty, gap_open_penalties, gap_extension_penalties, min_num_seeds_required_for_mapping, max_seed_frequencies, max_num_best_mappings, max_insert_size, num_threads, min_read_length, multi_mapping_allocation_distance, multi_mapping_allocation_seed, drop_repetitive_reads, trim_adapters, remove_pcr_duplicates, is_bulk_data, allocate_multi_mappings, only_output_unique_mappings, Tn5_shift, output_mapping_in_BED, output_mapping_in_TagAlign, output_mapping_in_PAF, reference_file_path, index_file_path, read_file1_path, read_file2_path, barcode_file_path, output_file_path);
          chromap_for_mapping.MapSingleEndReads();
        }
      }
    } else {
      if (output_mapping_in_PAF) {
        chromap::Chromap<chromap::PairedPAFMapping> chromap_for_mapping(error_threshold, match_score, mismatch_penalty, gap_open_penalties, gap_extension_penalties, min_num_seeds_required_for_mapping, max_seed_frequencies, max_num_best_mappings, max_insert_size, num_threads, min_read_length, multi_mapping_allocation_distance, multi_mapping_allocation_seed, drop_repetitive_reads, trim_adapters, remove_pcr_duplicates, is_bulk_data, allocate_multi_mappings, only_output_unique_mappings, Tn5_shift, output_mapping_in_BED, output_mapping_in_TagAlign, output_mapping_in_PAF, reference_file_path, index_file_path, read_file1_path, read_file2_path, barcode_file_path, output_file_path);
        chromap_for_mapping.MapPairedEndReads();
      } else {
        if (result.count("b") != 0) {
          chromap::Chromap<chromap::PairedEndMappingWithBarcode> chromap_for_mapping(error_threshold, match_score, mismatch_penalty, gap_open_penalties, gap_extension_penalties, min_num_seeds_required_for_mapping, max_seed_frequencies, max_num_best_mappings, max_insert_size, num_threads, min_read_length, multi_mapping_allocation_distance, multi_mapping_allocation_seed, drop_repetitive_reads, trim_adapters, remove_pcr_duplicates, is_bulk_data, allocate_multi_mappings, only_output_unique_mappings, Tn5_shift, output_mapping_in_BED, output_mapping_in_TagAlign, output_mapping_in_PAF, reference_file_path, index_file_path, read_file1_path, read_file2_path, barcode_file_path, output_file_path);
          chromap_for_mapping.MapPairedEndReads();
        } else {
          chromap::Chromap<chromap::PairedEndMappingWithoutBarcode> chromap_for_mapping(error_threshold, match_score, mismatch_penalty, gap_open_penalties, gap_extension_penalties, min_num_seeds_required_for_mapping, max_seed_frequencies, max_num_best_mappings, max_insert_size, num_threads, min_read_length, multi_mapping_allocation_distance, multi_mapping_allocation_seed, drop_repetitive_reads, trim_adapters, remove_pcr_duplicates, is_bulk_data, allocate_multi_mappings, only_output_unique_mappings, Tn5_shift, output_mapping_in_BED, output_mapping_in_TagAlign, output_mapping_in_PAF, reference_file_path, index_file_path, read_file1_path, read_file2_path, barcode_file_path, output_file_path);
          chromap_for_mapping.MapPairedEndReads();
        }
      }
    }
  } else if (result.count("h")) {
    std::cerr << options.help({"", "Indexing", "Mapping", "Input", "Output"});
  } else {
    std::cerr << options.help({"", "Indexing", "Mapping", "Input", "Output"});
  }
  return 0;
} 
