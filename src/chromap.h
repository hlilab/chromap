#ifndef CHROMAP_H_
#define CHROMAP_H_

#include <omp.h>

#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include <queue> // Used these two for k-minhash
#include <unordered_set>

#include <sstream> // Used for frip est params splitting

#include "candidate_processor.h"
#include "cxxopts.hpp"
#include "draft_mapping_generator.h"
#include "feature_barcode_matrix.h"
#include "index.h"
#include "index_parameters.h"
#include "khash.h"
#include "mapping_generator.h"
#include "mapping_metadata.h"
#include "mapping_parameters.h"
#include "mapping_processor.h"
#include "mapping_writer.h"
#include "minimizer_generator.h"
#include "mmcache.hpp"
#include "paired_end_mapping_metadata.h"
#include "sequence_batch.h"
#include "sequence_effective_range.h"
#include "temp_mapping.h"
#include "utils.h"

#define CHROMAP_VERSION "0.3.2-r518"

namespace chromap {

class K_MinHash {
public:
    /*
     * MinHash Class - used to estimate the number of unique cache slots 
     *                 hit by each barcode
     *
     * @param k - size of MinHash sketch
     * @param range - range of possible cache ids
     */
    K_MinHash(size_t k, size_t range) : k_(k), range_(range) {}

    inline void add(size_t num) {
      /* If num is not present in queue, we will add it */
        if (unique_slots_.find(num) == unique_slots_.end()) {
            unique_slots_.insert(num);
            pq_.push(num);
            // only keep smallest k numbers
            if (pq_.size() > k_) {
                unique_slots_.erase(pq_.top());
                pq_.pop();
            }
        }
    }

    inline size_t compute_cardinality() {
      /* Use k-MinHash estimator to return estimated cardinality */
      if (pq_.size() < k_) {return 0;}
      size_t cardinality = (k_ * range_)/pq_.top() - 1;
      return cardinality;
    }

private:
    size_t k_;
    size_t range_;

    /* Uses an unordered set to have O(1) find queries*/
    std::priority_queue<uint32_t> pq_; // max-heap
    std::unordered_set<uint32_t> unique_slots_; // keep track of unique values
};

class Chromap {
 public:
  Chromap() = delete;

  // For index construction
  Chromap(const IndexParameters &index_parameters)
      : index_parameters_(index_parameters) {
    barcode_lookup_table_ = NULL;
    barcode_whitelist_lookup_table_ = NULL;
  }

  // For mapping
  Chromap(const MappingParameters &mapping_parameters)
      : mapping_parameters_(mapping_parameters) {
    barcode_lookup_table_ = kh_init(k64_seq);
    barcode_whitelist_lookup_table_ = kh_init(k64_seq);

    ParseReadFormat(mapping_parameters.read_format);
  }

  ~Chromap() {
    if (barcode_whitelist_lookup_table_ != NULL) {
      kh_destroy(k64_seq, barcode_whitelist_lookup_table_);
    }

    if (barcode_lookup_table_ != NULL) {
      kh_destroy(k64_seq, barcode_lookup_table_);
    }
    if (read_lookup_tables_.size() > 0) {
      for (uint32_t i = 0; i < read_lookup_tables_.size(); ++i) {
        kh_destroy(k128, read_lookup_tables_[i]);
      }
    }
  }

  void ConstructIndex();

  template <typename MappingRecord>
  void MapSingleEndReads();

  template <typename MappingRecord>
  void MapPairedEndReads();

 private:
  uint32_t LoadSingleEndReadsWithBarcodes(SequenceBatch &read_batch,
                                          SequenceBatch &barcode_batch,
                                          bool parallel_parsing);

  uint32_t LoadPairedEndReadsWithBarcodes(SequenceBatch &read_batch1,
                                          SequenceBatch &read_batch2,
                                          SequenceBatch &barcode_batch,
                                          bool parallel_parsing);

  void TrimAdapterForPairedEndRead(uint32_t pair_index,
                                   SequenceBatch &read_batch1,
                                   SequenceBatch &read_batch2);

  bool PairedEndReadWithBarcodeIsDuplicate(uint32_t pair_index,
                                           const SequenceBatch &barcode_batch,
                                           const SequenceBatch &read_batch1,
                                           const SequenceBatch &read_batch2);

  uint32_t SampleInputBarcodesAndExamineLength();

  void LoadBarcodeWhitelist();

  void ComputeBarcodeAbundance(uint64_t max_num_sample_barcodes);

  void UpdateBarcodeAbundance(uint32_t num_loaded_barcodes,
                              const SequenceBatch &barcode_batch);

  bool CorrectBarcodeAt(uint32_t barcode_index, SequenceBatch &barcode_batch,
                        uint64_t &num_barcode_in_whitelist,
                        uint64_t &num_corrected_barcode);

  void OutputBarcodeStatistics();

  void OutputMappingStatistics();

  void ParseReadFormat(const std::string &read_format);

  // User custom rid order file contains a column of reference sequence names
  // and there is one name on each row. The reference sequence name on the ith
  // row means the rank of this sequence is i. This function loads the custom
  // rid order file and generates a mapping from the original rids to their
  // custom ranks, e.g., rid_ranks[i] is the custom rank of the ith rid in the
  // reference.
  void GenerateCustomRidRanks(const std::string &custom_rid_order_file_path,
                              uint32_t num_reference_sequences,
                              const SequenceBatch &reference,
                              std::vector<int> &rid_ranks);

  // TODO: generate reranked candidates directly.
  void RerankCandidatesRid(std::vector<Candidate> &candidates);

  // Parameters
  const IndexParameters index_parameters_;
  const MappingParameters mapping_parameters_;

  // Default batch size, # reads for single-end reads, # read pairs for
  // paired-end reads.
  const uint32_t read_batch_size_ = 500000;

  // 0-start, 1-end (includsive), 2-strand(-1:minus, 1:plus)
  SequenceEffectiveRange barcode_effective_range_;
  SequenceEffectiveRange read1_effective_range_;
  SequenceEffectiveRange read2_effective_range_;

  std::vector<int> custom_rid_rank_;
  std::vector<int> pairs_custom_rid_rank_;

  khash_t(k64_seq) * barcode_whitelist_lookup_table_;

  // For identical read dedupe
  khash_t(k64_seq) * barcode_lookup_table_;
  std::vector<khash_t(k128) *> read_lookup_tables_;

  // For mapping.
  const int min_unique_mapping_mapq_ = 4;

  // For mapping stats.
  uint64_t num_candidates_ = 0;
  uint64_t num_mappings_ = 0;
  uint64_t num_mapped_reads_ = 0;
  uint64_t num_uniquely_mapped_reads_ = 0;
  uint64_t num_reads_ = 0;
  // # identical reads.
  // uint64_t num_duplicated_reads_ = 0;

  // For barcode stats.
  const uint64_t initial_num_sample_barcodes_ = 20000000;
  uint64_t num_sample_barcodes_ = 0;
  uint64_t num_barcode_in_whitelist_ = 0;
  uint64_t num_corrected_barcode_ = 0;
  uint32_t barcode_length_ = 0;
};

template <typename MappingRecord>
void Chromap::MapSingleEndReads() {
  double real_start_time = GetRealTime();

  SequenceBatch reference;
  reference.InitializeLoading(mapping_parameters_.reference_file_path);
  reference.LoadAllSequences();
  uint32_t num_reference_sequences = reference.GetNumSequences();
  if (mapping_parameters_.custom_rid_order_file_path.length() > 0) {
    GenerateCustomRidRanks(mapping_parameters_.custom_rid_order_file_path,
                           num_reference_sequences, reference,
                           custom_rid_rank_);
    reference.ReorderSequences(custom_rid_rank_);
  }

  Index index(mapping_parameters_.index_file_path);
  index.Load();
  const int kmer_size = index.GetKmerSize();
  const int window_size = index.GetWindowSize();
  // index.Statistics(num_sequences, reference);

  SequenceBatch read_batch(read_batch_size_, read1_effective_range_);
  SequenceBatch read_batch_for_loading(read_batch_size_,
                                       read1_effective_range_);
  SequenceBatch barcode_batch(read_batch_size_, barcode_effective_range_);
  SequenceBatch barcode_batch_for_loading(read_batch_size_,
                                          barcode_effective_range_);

  std::vector<std::vector<MappingRecord>> mappings_on_diff_ref_seqs;
  mappings_on_diff_ref_seqs.reserve(num_reference_sequences);
  for (uint32_t i = 0; i < num_reference_sequences; ++i) {
    mappings_on_diff_ref_seqs.emplace_back(std::vector<MappingRecord>());
  }

  std::vector<TempMappingFileHandle<MappingRecord>> temp_mapping_file_handles;

  // Preprocess barcodes for single cell data
  if (!mapping_parameters_.is_bulk_data) {
    barcode_length_ = SampleInputBarcodesAndExamineLength();
    if (!mapping_parameters_.barcode_whitelist_file_path.empty()) {
      LoadBarcodeWhitelist();
      ComputeBarcodeAbundance(initial_num_sample_barcodes_);
    }
  }

  MinimizerGenerator minimizer_generator(kmer_size, window_size);

  CandidateProcessor candidate_processor(
      mapping_parameters_.min_num_seeds_required_for_mapping,
      mapping_parameters_.max_seed_frequencies);

  MappingProcessor<MappingRecord> mapping_processor(mapping_parameters_,
                                                    min_unique_mapping_mapq_);

  DraftMappingGenerator draft_mapping_generator(mapping_parameters_);

  MappingGenerator<MappingRecord> mapping_generator(mapping_parameters_,
                                                    pairs_custom_rid_rank_);

  MappingWriter<MappingRecord> mapping_writer(
      mapping_parameters_, barcode_length_, pairs_custom_rid_rank_);

  mapping_writer.OutputHeader(num_reference_sequences, reference);

  uint32_t num_mappings_in_mem = 0;
  uint64_t max_num_mappings_in_mem =
      1 * ((uint64_t)1 << 30) / sizeof(MappingRecord);
  if (mapping_parameters_.mapping_output_format == MAPPINGFORMAT_SAM ||
      mapping_parameters_.mapping_output_format == MAPPINGFORMAT_PAF ||
      mapping_parameters_.mapping_output_format == MAPPINGFORMAT_PAIRS) {
    max_num_mappings_in_mem = 1 * ((uint64_t)1 << 29) / sizeof(MappingRecord);
  }

  mm_cache mm_to_candidates_cache(2000003);
  mm_to_candidates_cache.SetKmerLength(kmer_size);
  struct _mm_history *mm_history = new struct _mm_history[read_batch_size_];
  // Use bit encoding to represent mapping results
  // bit 0: is barcode in whitelist
  uint8_t *read_map_summary = NULL ; 
  if (!mapping_parameters_.summary_metadata_file_path.empty()) {
    read_map_summary = new uint8_t[read_batch_size_];
    memset(read_map_summary, 1, sizeof(*read_map_summary)*read_batch_size_);
  }

  static uint64_t thread_num_candidates = 0;
  static uint64_t thread_num_mappings = 0;
  static uint64_t thread_num_mapped_reads = 0;
  static uint64_t thread_num_uniquely_mapped_reads = 0;
  static uint64_t thread_num_barcode_in_whitelist = 0;
  static uint64_t thread_num_corrected_barcode = 0;
#pragma omp threadprivate(                                               \
    thread_num_candidates, thread_num_mappings, thread_num_mapped_reads, \
    thread_num_uniquely_mapped_reads, thread_num_barcode_in_whitelist,   \
    thread_num_corrected_barcode)
  double real_start_mapping_time = GetRealTime();
  for (size_t read_file_index = 0;
       read_file_index < mapping_parameters_.read_file1_paths.size();
       ++read_file_index) {
    read_batch_for_loading.InitializeLoading(
        mapping_parameters_.read_file1_paths[read_file_index]);

    if (!mapping_parameters_.is_bulk_data) {
      barcode_batch_for_loading.InitializeLoading(
          mapping_parameters_.barcode_file_paths[read_file_index]);
    }

    uint32_t num_loaded_reads_for_loading = 0;
    uint32_t num_loaded_reads = LoadSingleEndReadsWithBarcodes(
        read_batch_for_loading, barcode_batch_for_loading,
        mapping_parameters_.num_threads >= 3 ? true : false);
    read_batch_for_loading.SwapSequenceBatch(read_batch);

    if (!mapping_parameters_.is_bulk_data) {
      barcode_batch_for_loading.SwapSequenceBatch(barcode_batch);
    }

    std::vector<std::vector<std::vector<MappingRecord>>>
        mappings_on_diff_ref_seqs_for_diff_threads;
    std::vector<std::vector<std::vector<MappingRecord>>>
        mappings_on_diff_ref_seqs_for_diff_threads_for_saving;
    mappings_on_diff_ref_seqs_for_diff_threads.reserve(
        mapping_parameters_.num_threads);
    mappings_on_diff_ref_seqs_for_diff_threads_for_saving.reserve(
        mapping_parameters_.num_threads);
    for (int ti = 0; ti < mapping_parameters_.num_threads; ++ti) {
      mappings_on_diff_ref_seqs_for_diff_threads.emplace_back(
          std::vector<std::vector<MappingRecord>>(num_reference_sequences));
      mappings_on_diff_ref_seqs_for_diff_threads_for_saving.emplace_back(
          std::vector<std::vector<MappingRecord>>(num_reference_sequences));
      for (uint32_t i = 0; i < num_reference_sequences; ++i) {
        mappings_on_diff_ref_seqs_for_diff_threads[ti][i].reserve(
            (num_loaded_reads + num_loaded_reads / 1000 *
                                    mapping_parameters_.max_num_best_mappings) /
            mapping_parameters_.num_threads / num_reference_sequences);
        mappings_on_diff_ref_seqs_for_diff_threads_for_saving[ti][i].reserve(
            (num_loaded_reads + num_loaded_reads / 1000 *
                                    mapping_parameters_.max_num_best_mappings) /
            mapping_parameters_.num_threads / num_reference_sequences);
      }
    }
#pragma omp parallel shared(num_reads_, mm_history, read_map_summary, reference, index, read_batch, barcode_batch, read_batch_for_loading, barcode_batch_for_loading, std::cerr, num_loaded_reads_for_loading, num_loaded_reads, num_reference_sequences, mappings_on_diff_ref_seqs_for_diff_threads, mappings_on_diff_ref_seqs_for_diff_threads_for_saving, mappings_on_diff_ref_seqs, temp_mapping_file_handles, mm_to_candidates_cache, mapping_writer, minimizer_generator, candidate_processor, mapping_processor, draft_mapping_generator, mapping_generator, num_mappings_in_mem, max_num_mappings_in_mem) num_threads(mapping_parameters_.num_threads) reduction(+:num_candidates_, num_mappings_, num_mapped_reads_, num_uniquely_mapped_reads_, num_barcode_in_whitelist_, num_corrected_barcode_)
    {
      thread_num_candidates = 0;
      thread_num_mappings = 0;
      thread_num_mapped_reads = 0;
      thread_num_uniquely_mapped_reads = 0;
      thread_num_barcode_in_whitelist = 0;
      thread_num_corrected_barcode = 0;
      MappingMetadata mapping_metadata;
#pragma omp single
      {
        while (num_loaded_reads > 0) {
          double real_batch_start_time = GetRealTime();
          num_reads_ += num_loaded_reads;
#pragma omp task
          {
            num_loaded_reads_for_loading = LoadSingleEndReadsWithBarcodes(
                read_batch_for_loading, barcode_batch_for_loading,
                mapping_parameters_.num_threads >= 12 ? true : false);
          }  // end of openmp loading task
          uint32_t history_update_threshold =
          mm_to_candidates_cache.GetUpdateThreshold(num_loaded_reads,
                                                    num_reads_, 
                                                    false,
                                                    0.01);
          // int grain_size = 10000;
//#pragma omp taskloop grainsize(grain_size) //num_tasks(num_threads_* 50)
#pragma omp taskloop num_tasks( \
    mapping_parameters_.num_threads *mapping_parameters_.num_threads)
          for (uint32_t read_index = 0; read_index < num_loaded_reads;
               ++read_index) {
            bool current_barcode_is_whitelisted = true;
            if (!mapping_parameters_.barcode_whitelist_file_path.empty()) {
              current_barcode_is_whitelisted = CorrectBarcodeAt(
                  read_index, barcode_batch, thread_num_barcode_in_whitelist,
                  thread_num_corrected_barcode);
            }

            if (!(current_barcode_is_whitelisted ||
                  mapping_parameters_.output_mappings_not_in_whitelist)) {
              if (read_map_summary != NULL)
                read_map_summary[read_index] = 0;
              continue;
            }
            
            if (read_batch.GetSequenceLengthAt(read_index) <
                (uint32_t)mapping_parameters_.min_read_length) {
              continue;  // reads are too short, just drop.
            }

            read_batch.PrepareNegativeSequenceAt(read_index);

            mapping_metadata.PrepareForMappingNextRead(
                mapping_parameters_.max_seed_frequencies[0]);

            minimizer_generator.GenerateMinimizers(
                read_batch, read_index, mapping_metadata.minimizers_);

            if (mapping_metadata.minimizers_.size() > 0) {
              if (mapping_parameters_.custom_rid_order_file_path.length() > 0) {
                RerankCandidatesRid(mapping_metadata.positive_candidates_);
                RerankCandidatesRid(mapping_metadata.negative_candidates_);
              }

              if (mm_to_candidates_cache.Query(
                      mapping_metadata,
                      read_batch.GetSequenceLengthAt(read_index)) == -1) {
                candidate_processor.GenerateCandidates(
                    mapping_parameters_.error_threshold, index,
                    mapping_metadata);
              }

              if (read_index < history_update_threshold) {
                mm_history[read_index].timestamp = num_reads_;
                mm_history[read_index].minimizers =
                    mapping_metadata.minimizers_;
                mm_history[read_index].positive_candidates =
                    mapping_metadata.positive_candidates_;
                mm_history[read_index].negative_candidates =
                    mapping_metadata.negative_candidates_;
                mm_history[read_index].repetitive_seed_length =
                    mapping_metadata.repetitive_seed_length_;
              }

              size_t current_num_candidates =
                  mapping_metadata.GetNumCandidates();
              if (current_num_candidates > 0) {
                thread_num_candidates += current_num_candidates;
                draft_mapping_generator.GenerateDraftMappings(
                    read_batch, read_index, reference, mapping_metadata);

                const size_t current_num_draft_mappings =
                    mapping_metadata.GetNumDraftMappings();
                if (current_num_draft_mappings > 0) {
                  std::vector<std::vector<MappingRecord>>
                      &mappings_on_diff_ref_seqs =
                          mappings_on_diff_ref_seqs_for_diff_threads
                              [omp_get_thread_num()];

                  mapping_generator.GenerateBestMappingsForSingleEndRead(
                      read_batch, read_index, reference, barcode_batch,
                      mapping_metadata, mappings_on_diff_ref_seqs);

                  thread_num_mappings +=
                      std::min(mapping_metadata.GetNumBestMappings(),
                               mapping_parameters_.max_num_best_mappings);
                  ++thread_num_mapped_reads;

                  if (mapping_metadata.GetNumBestMappings() == 1) {
                    ++thread_num_uniquely_mapped_reads;
                  }
                }
              }
            }
          }
#pragma omp taskwait
          for (uint32_t read_index = 0; read_index < history_update_threshold;
               ++read_index) {
            if (mm_history[read_index].timestamp != num_reads_) continue;
            mm_to_candidates_cache.Update(
                mm_history[read_index].minimizers,
                mm_history[read_index].positive_candidates,
                mm_history[read_index].negative_candidates,
                mm_history[read_index].repetitive_seed_length);
            if (mm_history[read_index].positive_candidates.size() <
                mm_history[read_index].positive_candidates.capacity() / 2) {
              std::vector<Candidate>().swap(
                  mm_history[read_index].positive_candidates);
            }
            if (mm_history[read_index].negative_candidates.size() <
                mm_history[read_index].negative_candidates.capacity() / 2) {
              std::vector<Candidate>().swap(
                  mm_history[read_index].negative_candidates);
            }
          }
          // std::cerr<<"cache memusage: " <<
          // mm_to_candidates_cache.GetMemoryBytes() <<"\n" ;
          if (!mapping_parameters_.summary_metadata_file_path.empty()) {
            if (mapping_parameters_.is_bulk_data) 
              mapping_writer.UpdateSummaryMetadata(0, SUMMARY_METADATA_TOTAL, 
                  num_loaded_reads) ;
            else {
              uint32_t nonwhitelist_count = 0;
              for (uint32_t read_index = 0; read_index < num_loaded_reads; ++read_index)
                if (read_map_summary[read_index] & 1) {
                  mapping_writer.UpdateSummaryMetadata(
                      barcode_batch.GenerateSeedFromSequenceAt(read_index, 0, barcode_length_), 
                      SUMMARY_METADATA_TOTAL, 1);
                } else {
                  ++nonwhitelist_count;
                }
              
              mapping_writer.UpdateSpeicalCategorySummaryMetadata(/*nonwhitelist*/0, 
                  SUMMARY_METADATA_TOTAL, nonwhitelist_count);
            }

            // By default, set the lowest bit to 1 (whether the barcode is in the whitelist)
            memset(read_map_summary, 1, sizeof(*read_map_summary)*read_batch_size_);
          }
          num_loaded_reads = num_loaded_reads_for_loading;
          read_batch_for_loading.SwapSequenceBatch(read_batch);
          barcode_batch_for_loading.SwapSequenceBatch(barcode_batch);
          mappings_on_diff_ref_seqs_for_diff_threads.swap(
              mappings_on_diff_ref_seqs_for_diff_threads_for_saving);
#pragma omp task
          {
            num_mappings_in_mem +=
                mapping_processor.MoveMappingsInBuffersToMappingContainer(
                    num_reference_sequences,
                    mappings_on_diff_ref_seqs_for_diff_threads_for_saving,
                    mappings_on_diff_ref_seqs);
            if (mapping_parameters_.low_memory_mode &&
                num_mappings_in_mem > max_num_mappings_in_mem) {
              mapping_processor.ParallelSortOutputMappings(num_reference_sequences,
                                                   mappings_on_diff_ref_seqs, 0);

              mapping_writer.OutputTempMappings(num_reference_sequences,
                                                mappings_on_diff_ref_seqs,
                                                temp_mapping_file_handles);

              if (temp_mapping_file_handles.size() > 850
                  && temp_mapping_file_handles.size() % 10 == 1) { // every 10 temp files, double the temp file size
                max_num_mappings_in_mem <<= 1;
                std::cerr << "Used " << temp_mapping_file_handles.size() << "temp files. Double the temp file volume to " << max_num_mappings_in_mem << "\n" ;
              }
              num_mappings_in_mem = 0;
            }
          }
          std::cerr << "Mapped " << num_loaded_reads << " reads in "
                    << GetRealTime() - real_batch_start_time << "s.\n";
        }
      }  // end of openmp single
      {
        num_barcode_in_whitelist_ += thread_num_barcode_in_whitelist;
        num_corrected_barcode_ += thread_num_corrected_barcode;
        num_candidates_ += thread_num_candidates;
        num_mappings_ += thread_num_mappings;
        num_mapped_reads_ += thread_num_mapped_reads;
        num_uniquely_mapped_reads_ += thread_num_uniquely_mapped_reads;
      }  // end of updating shared mapping stats
    }    // end of openmp parallel region
    read_batch_for_loading.FinalizeLoading();
    if (!mapping_parameters_.is_bulk_data) {
      barcode_batch_for_loading.FinalizeLoading();
    }
  }

  std::cerr << "Mapped all reads in " << GetRealTime() - real_start_mapping_time
            << "s.\n";

  delete[] mm_history;
  if (read_map_summary != NULL)
    delete[] read_map_summary;

  OutputMappingStatistics();
  if (!mapping_parameters_.is_bulk_data) {
    OutputBarcodeStatistics();
  }

  index.Destroy();

  if (mapping_parameters_.low_memory_mode) {
    // First, process the remaining mappings in the memory and save them on
    // disk.
    if (num_mappings_in_mem > 0) {
      mapping_processor.SortOutputMappings(num_reference_sequences,
                                           mappings_on_diff_ref_seqs);

      mapping_writer.OutputTempMappings(num_reference_sequences,
                                        mappings_on_diff_ref_seqs,
                                        temp_mapping_file_handles);
      num_mappings_in_mem = 0;
    }

    mapping_writer.ProcessAndOutputMappingsInLowMemory(
        num_mappings_in_mem, num_reference_sequences, reference,
        barcode_whitelist_lookup_table_, temp_mapping_file_handles);
  } else {
    if (mapping_parameters_.Tn5_shift) {
      mapping_processor.ApplyTn5ShiftOnMappings(num_reference_sequences,
                                                mappings_on_diff_ref_seqs);
    }

    if (mapping_parameters_.remove_pcr_duplicates) {
      mapping_processor.RemovePCRDuplicate(num_reference_sequences,
                                           mappings_on_diff_ref_seqs,
                                           mapping_parameters_.num_threads);
      std::cerr << "After removing PCR duplications, ";
      mapping_processor.OutputMappingStatistics(num_reference_sequences,
                                                mappings_on_diff_ref_seqs);
    } else {
      mapping_processor.ParallelSortOutputMappings(num_reference_sequences,
                                           mappings_on_diff_ref_seqs,
                                           mapping_parameters_.num_threads);
    }

    if (mapping_parameters_.allocate_multi_mappings) {
      const uint64_t num_multi_mappings =
          num_mapped_reads_ - num_uniquely_mapped_reads_;
      mapping_processor.AllocateMultiMappings(
          num_reference_sequences, num_multi_mappings,
          mapping_parameters_.multi_mapping_allocation_distance,
          mappings_on_diff_ref_seqs);
      std::cerr << "After allocating multi-mappings, ";
      mapping_processor.OutputMappingStatistics(num_reference_sequences,
                                                mappings_on_diff_ref_seqs);
      mapping_processor.SortOutputMappings(num_reference_sequences,
                                           mappings_on_diff_ref_seqs);
    }
    mapping_writer.OutputMappings(num_reference_sequences, reference,
                                  mappings_on_diff_ref_seqs);
  }
  mapping_writer.OutputSummaryMetadata();

  reference.FinalizeLoading();
  std::cerr << "Total time: " << GetRealTime() - real_start_time << "s.\n";
}

template <typename MappingRecord>
void Chromap::MapPairedEndReads() {
  double real_start_time = GetRealTime();

  // Load reference
  SequenceBatch reference;
  reference.InitializeLoading(mapping_parameters_.reference_file_path);
  reference.LoadAllSequences();
  uint32_t num_reference_sequences = reference.GetNumSequences();
  
  // Debugging Info (printing out reference information)
  if (mapping_parameters_.debug_cache) {
    for (size_t i = 0; i < num_reference_sequences; i++){
      std::cout << "[DEBUG][INDEX] seq_i = " << i 
                << " , seq_i_name = " << reference.GetSequenceNameAt(i) << std::endl;
    }
  }
  
  if (mapping_parameters_.custom_rid_order_file_path.length() > 0) {
    GenerateCustomRidRanks(mapping_parameters_.custom_rid_order_file_path,
                           num_reference_sequences, reference,
                           custom_rid_rank_);
    reference.ReorderSequences(custom_rid_rank_);
  }
  if (mapping_parameters_.mapping_output_format == MAPPINGFORMAT_PAIRS) {
    GenerateCustomRidRanks(
        mapping_parameters_.pairs_flipping_custom_rid_order_file_path,
        num_reference_sequences, reference, pairs_custom_rid_rank_);
  }

  // Load index
  Index index(mapping_parameters_.index_file_path);
  index.Load();
  const int kmer_size = index.GetKmerSize();
  const int window_size = index.GetWindowSize();
  // index.Statistics(num_sequences, reference);

  // Initialize read batches
  SequenceBatch read_batch1(read_batch_size_, read1_effective_range_);
  SequenceBatch read_batch2(read_batch_size_, read2_effective_range_);
  SequenceBatch barcode_batch(read_batch_size_, barcode_effective_range_);
  SequenceBatch read_batch1_for_loading(read_batch_size_,
                                        read1_effective_range_);
  SequenceBatch read_batch2_for_loading(read_batch_size_,
                                        read2_effective_range_);
  SequenceBatch barcode_batch_for_loading(read_batch_size_,
                                          barcode_effective_range_);

  // Check cache-related parameters
  std::cerr << "Cache Size: " << mapping_parameters_.cache_size << std::endl;
  std::cerr << "Cache Update Param: " << mapping_parameters_.cache_update_param << std::endl;
  
  std::vector<uint64_t> seeds_for_batch(500000, 0);

  // Variables used for counting number of associated cache slots
  bool output_num_cache_slots_info = mapping_parameters_.output_num_uniq_cache_slots;
  if (mapping_parameters_.summary_metadata_file_path.empty()) {
    output_num_cache_slots_info = false;
  }
  const size_t k_for_minhash = mapping_parameters_.k_for_minhash;

  std::cerr << "Output number of associated cache slots: " << output_num_cache_slots_info << std::endl;
  std::cerr << "K for MinHash: " << k_for_minhash << std::endl;

  int num_locks_for_map = 1000;
  omp_lock_t map_locks[num_locks_for_map];
  for (int i = 0; i < num_locks_for_map; ++i) {omp_init_lock(&map_locks[i]);}
  
  std::vector<std::unordered_map<size_t, K_MinHash>> barcode_peak_map(num_locks_for_map);

  // Parse out the parameters for chromap score (const, fric, dup, unmapped, lowmapq)
  std::vector<double> frip_est_params; 
  std::stringstream ss(mapping_parameters_.frip_est_params);
  std::string token;

  while(std::getline(ss, token, ';')) {
    try {
      auto curr_param = std::stod(token);
      frip_est_params.push_back(curr_param);
    } catch(...) {
      chromap::ExitWithMessage(
        "\nException occurred while processing chromap score parameters\n"
        );
    }
  }
  if (frip_est_params.size() != 5) {
    chromap::ExitWithMessage(
      "\nInvalid number of parameters, expecting 5 parameters but found " 
      + std::to_string(frip_est_params.size()) 
      + " parameters\n"
      );
  }

  // Initialize vector to keep track of cache hits for each thread
  std::vector<int> cache_hits_per_thread(mapping_parameters_.num_threads, 0);

  // Initialize cache
  mm_cache mm_to_candidates_cache(mapping_parameters_.cache_size);
  mm_to_candidates_cache.SetKmerLength(kmer_size);

  struct _mm_history *mm_history1 = new struct _mm_history[read_batch_size_];
  struct _mm_history *mm_history2 = new struct _mm_history[read_batch_size_];
  
  // The explanation for read_map_summary is in the single-end mapping function
  uint8_t *read_map_summary = NULL ;
  if (!mapping_parameters_.summary_metadata_file_path.empty()) {
    read_map_summary = new uint8_t[read_batch_size_];
    memset(read_map_summary, 1, sizeof(*read_map_summary)*read_batch_size_);
  }
  std::vector<std::vector<MappingRecord>> mappings_on_diff_ref_seqs;
  
  // Initialize mapping container
  mappings_on_diff_ref_seqs.reserve(num_reference_sequences);
  for (uint32_t i = 0; i < num_reference_sequences; ++i) {
    mappings_on_diff_ref_seqs.emplace_back(std::vector<MappingRecord>());
  }
  std::vector<TempMappingFileHandle<MappingRecord>> temp_mapping_file_handles;

  // Preprocess barcodes for single cell data
  if (!mapping_parameters_.is_bulk_data) {
    barcode_length_ = SampleInputBarcodesAndExamineLength();
    if (!mapping_parameters_.barcode_whitelist_file_path.empty()) {
      LoadBarcodeWhitelist();
      ComputeBarcodeAbundance(initial_num_sample_barcodes_);
    }
  }

  MinimizerGenerator minimizer_generator(kmer_size, window_size);

  CandidateProcessor candidate_processor(
      mapping_parameters_.min_num_seeds_required_for_mapping,
      mapping_parameters_.max_seed_frequencies);

  MappingProcessor<MappingRecord> mapping_processor(mapping_parameters_,
                                                    min_unique_mapping_mapq_);

  DraftMappingGenerator draft_mapping_generator(mapping_parameters_);

  MappingGenerator<MappingRecord> mapping_generator(mapping_parameters_,
                                                    pairs_custom_rid_rank_);

  MappingWriter<MappingRecord> mapping_writer(
      mapping_parameters_, barcode_length_, pairs_custom_rid_rank_);
  mapping_writer.OutputHeader(num_reference_sequences, reference);

  uint32_t num_mappings_in_mem = 0;
  uint64_t max_num_mappings_in_mem =
      1 * ((uint64_t)1 << 30) / sizeof(MappingRecord);
  if (mapping_parameters_.mapping_output_format == MAPPINGFORMAT_SAM ||
      mapping_parameters_.mapping_output_format == MAPPINGFORMAT_PAF ||
      mapping_parameters_.mapping_output_format == MAPPINGFORMAT_PAIRS) {
    max_num_mappings_in_mem = 1 * ((uint64_t)1 << 29) / sizeof(MappingRecord);
  }

  static uint64_t thread_num_candidates = 0;
  static uint64_t thread_num_mappings = 0;
  static uint64_t thread_num_mapped_reads = 0;
  static uint64_t thread_num_uniquely_mapped_reads = 0;
  static uint64_t thread_num_barcode_in_whitelist = 0;
  static uint64_t thread_num_corrected_barcode = 0;
#pragma omp threadprivate(                                               \
    thread_num_candidates, thread_num_mappings, thread_num_mapped_reads, \
    thread_num_uniquely_mapped_reads, thread_num_barcode_in_whitelist,   \
    thread_num_corrected_barcode)
  double real_start_mapping_time = GetRealTime();
  for (size_t read_file_index = 0;
       read_file_index < mapping_parameters_.read_file1_paths.size();
       ++read_file_index) {
    // Set read batches to the current read files.
    read_batch1_for_loading.InitializeLoading(
        mapping_parameters_.read_file1_paths[read_file_index]);
    read_batch2_for_loading.InitializeLoading(
        mapping_parameters_.read_file2_paths[read_file_index]);
    if (!mapping_parameters_.is_bulk_data) {
      barcode_batch_for_loading.InitializeLoading(
          mapping_parameters_.barcode_file_paths[read_file_index]);
    }

    // Load the first batches.
    uint32_t num_loaded_pairs_for_loading = 0;
    uint32_t num_loaded_pairs = LoadPairedEndReadsWithBarcodes(
        read_batch1_for_loading, read_batch2_for_loading,
        barcode_batch_for_loading, mapping_parameters_.num_threads >= 3 ? true : false);
    read_batch1_for_loading.SwapSequenceBatch(read_batch1);
    read_batch2_for_loading.SwapSequenceBatch(read_batch2);
    if (!mapping_parameters_.is_bulk_data) {
      barcode_batch_for_loading.SwapSequenceBatch(barcode_batch);
    }

    // Setup thread private vectors to save mapping results.
    std::vector<std::vector<std::vector<MappingRecord>>>
        mappings_on_diff_ref_seqs_for_diff_threads;
    std::vector<std::vector<std::vector<MappingRecord>>>
        mappings_on_diff_ref_seqs_for_diff_threads_for_saving;
    mappings_on_diff_ref_seqs_for_diff_threads.reserve(
        mapping_parameters_.num_threads);
    mappings_on_diff_ref_seqs_for_diff_threads_for_saving.reserve(
        mapping_parameters_.num_threads);
    for (int ti = 0; ti < mapping_parameters_.num_threads; ++ti) {
      mappings_on_diff_ref_seqs_for_diff_threads.emplace_back(
          std::vector<std::vector<MappingRecord>>(num_reference_sequences));
      mappings_on_diff_ref_seqs_for_diff_threads_for_saving.emplace_back(
          std::vector<std::vector<MappingRecord>>(num_reference_sequences));
      for (uint32_t i = 0; i < num_reference_sequences; ++i) {
        mappings_on_diff_ref_seqs_for_diff_threads[ti][i].reserve(
            (num_loaded_pairs + num_loaded_pairs / 1000 *
                                    mapping_parameters_.max_num_best_mappings) /
            mapping_parameters_.num_threads / num_reference_sequences);
        mappings_on_diff_ref_seqs_for_diff_threads_for_saving[ti][i].reserve(
            (num_loaded_pairs + num_loaded_pairs / 1000 *
                                    mapping_parameters_.max_num_best_mappings) /
            mapping_parameters_.num_threads / num_reference_sequences);
      }
    }

#pragma omp parallel shared(num_reads_, num_reference_sequences, reference, index, read_batch1, read_batch2, barcode_batch, read_batch1_for_loading, read_batch2_for_loading, barcode_batch_for_loading, minimizer_generator, candidate_processor, mapping_processor, draft_mapping_generator, mapping_generator, mapping_writer, std::cerr, num_loaded_pairs_for_loading, num_loaded_pairs, mappings_on_diff_ref_seqs_for_diff_threads, mappings_on_diff_ref_seqs_for_diff_threads_for_saving, mappings_on_diff_ref_seqs, num_mappings_in_mem, max_num_mappings_in_mem, temp_mapping_file_handles, mm_to_candidates_cache, mm_history1, mm_history2, read_map_summary) num_threads(mapping_parameters_.num_threads) reduction(+:num_candidates_, num_mappings_, num_mapped_reads_, num_uniquely_mapped_reads_, num_barcode_in_whitelist_, num_corrected_barcode_)
    {
      thread_num_candidates = 0;
      thread_num_mappings = 0;
      thread_num_mapped_reads = 0;
      thread_num_uniquely_mapped_reads = 0;
      thread_num_barcode_in_whitelist = 0;
      thread_num_corrected_barcode = 0;
      PairedEndMappingMetadata paired_end_mapping_metadata;

      std::vector<int> best_mapping_indices(
          mapping_parameters_.max_num_best_mappings);
      std::mt19937 generator(11);
#pragma omp single
      {
        double real_batch_start_time = GetRealTime();
        while (num_loaded_pairs > 0) {
          num_reads_ += num_loaded_pairs;
          num_reads_ += num_loaded_pairs;

#pragma omp task
          {
            num_loaded_pairs_for_loading = LoadPairedEndReadsWithBarcodes(
                read_batch1_for_loading, read_batch2_for_loading,
                barcode_batch_for_loading, 
                mapping_parameters_.num_threads >= 12 ? true : false);
          }  // end of openmp loading task

          int grain_size = 5000;
          uint32_t history_update_threshold =
          mm_to_candidates_cache.GetUpdateThreshold(num_loaded_pairs,
                                                    num_reads_, 
                                                    true,
                                                    mapping_parameters_.cache_update_param
                                                    );
          std::fill(cache_hits_per_thread.begin(), cache_hits_per_thread.end(), 0);

          if (mapping_parameters_.debug_cache) {
            std::cout << "[DEBUG][UPDATE] update_threshold = " << history_update_threshold << std::endl;
          }

#pragma omp taskloop grainsize(grain_size)
          for (uint32_t pair_index = 0; pair_index < num_loaded_pairs;
               ++pair_index) {
            int thread_id = omp_get_thread_num();
            
            bool current_barcode_is_whitelisted = true;
            if (!mapping_parameters_.barcode_whitelist_file_path.empty()) {
              current_barcode_is_whitelisted = CorrectBarcodeAt(
                  pair_index, barcode_batch, thread_num_barcode_in_whitelist,
                  thread_num_corrected_barcode);
            }

            // calculate seed value for each barcode to use later (below and summary update)
            size_t curr_seed_val = barcode_batch.GenerateSeedFromSequenceAt(pair_index, 0, barcode_length_);
            seeds_for_batch[pair_index] = curr_seed_val;

            if (current_barcode_is_whitelisted ||
                mapping_parameters_.output_mappings_not_in_whitelist) {
              
              if (read_batch1.GetSequenceLengthAt(pair_index) <
                  (uint32_t)mapping_parameters_.min_read_length ||
                  read_batch2.GetSequenceLengthAt(pair_index) <
                  (uint32_t)mapping_parameters_.min_read_length) {
                continue;  // reads are too short, just drop.
              }

              read_batch1.PrepareNegativeSequenceAt(pair_index);
              read_batch2.PrepareNegativeSequenceAt(pair_index);

              if (mapping_parameters_.trim_adapters) {
                TrimAdapterForPairedEndRead(pair_index, read_batch1,
                                            read_batch2);
              }

              paired_end_mapping_metadata.PreparedForMappingNextReadPair(
                  mapping_parameters_.max_seed_frequencies[0]);

              minimizer_generator.GenerateMinimizers(
                  read_batch1, pair_index,
                  paired_end_mapping_metadata.mapping_metadata1_.minimizers_);
              minimizer_generator.GenerateMinimizers(
                  read_batch2, pair_index,
                  paired_end_mapping_metadata.mapping_metadata2_.minimizers_);

              if (paired_end_mapping_metadata.BothEndsHaveMinimizers()) {
                // declare temp local variable for cache result
                int cache_query_result1 = 0;
                int cache_query_result2 = 0;
                int cache_miss = 0;

                cache_query_result1 = mm_to_candidates_cache.Query(paired_end_mapping_metadata.mapping_metadata1_,
                                                                  read_batch1.GetSequenceLengthAt(pair_index));
                if (cache_query_result1 == -1) 
                {
                  candidate_processor.GenerateCandidates(
                      mapping_parameters_.error_threshold, 
                      index,
                      paired_end_mapping_metadata.mapping_metadata1_
                      );
                  ++cache_miss;
                }
                size_t current_num_candidates1 = paired_end_mapping_metadata.mapping_metadata1_.GetNumCandidates();


                cache_query_result2 = mm_to_candidates_cache.Query(paired_end_mapping_metadata.mapping_metadata2_,
                                                                  read_batch2.GetSequenceLengthAt(pair_index));
                if (cache_query_result2 == -1) 
                {
                  candidate_processor.GenerateCandidates(
                      mapping_parameters_.error_threshold, 
                      index,
                      paired_end_mapping_metadata.mapping_metadata2_
                      );
                  ++cache_miss;
                }
                size_t current_num_candidates2 = paired_end_mapping_metadata.mapping_metadata2_.GetNumCandidates();

                // increment variable for cache_hits
                bool curr_read_hit_cache = false;
                if (cache_query_result1 >= 0 || cache_query_result2 >= 0) {
                  cache_hits_per_thread[thread_id]++;
                  curr_read_hit_cache = true;
                }

                // update the peak counting data-structure
                if (output_num_cache_slots_info && curr_read_hit_cache) {
                  // calculate which map this barcode is in
                  size_t map_id = curr_seed_val % num_locks_for_map;
                
                  // grab lock for this map, and add to the K-MinHash for this particular barcode
                  omp_set_lock(&map_locks[map_id]);
                  auto it = barcode_peak_map[map_id].emplace(curr_seed_val, K_MinHash(k_for_minhash, mapping_parameters_.cache_size)).first;
                  if (cache_query_result1 >= 0) {it->second.add(cache_query_result1);}
                  if (cache_query_result2 >= 0) {it->second.add(cache_query_result2);}
                  omp_unset_lock(&map_locks[map_id]);
                }

                if (pair_index < history_update_threshold) {
                  mm_history1[pair_index].timestamp =
                      mm_history2[pair_index].timestamp = num_reads_;
                  mm_history1[pair_index].minimizers =
                      paired_end_mapping_metadata.mapping_metadata1_
                          .minimizers_;
                  mm_history1[pair_index].positive_candidates =
                      paired_end_mapping_metadata.mapping_metadata1_
                          .positive_candidates_;
                  mm_history1[pair_index].negative_candidates =
                      paired_end_mapping_metadata.mapping_metadata1_
                          .negative_candidates_;
                  mm_history1[pair_index].repetitive_seed_length =
                      paired_end_mapping_metadata.mapping_metadata1_
                          .repetitive_seed_length_;
                  mm_history2[pair_index].minimizers =
                      paired_end_mapping_metadata.mapping_metadata2_
                          .minimizers_;
                  mm_history2[pair_index].positive_candidates =
                      paired_end_mapping_metadata.mapping_metadata2_
                          .positive_candidates_;
                  mm_history2[pair_index].negative_candidates =
                      paired_end_mapping_metadata.mapping_metadata2_
                          .negative_candidates_;
                  mm_history2[pair_index].repetitive_seed_length =
                      paired_end_mapping_metadata.mapping_metadata2_
                          .repetitive_seed_length_;
                }

                // Test whether we need to augment the candidate list with mate
                // information.
                int supplementCandidateResult = 0;
                if (!mapping_parameters_.split_alignment) {
                  supplementCandidateResult =
                      candidate_processor.SupplementCandidates(
                          mapping_parameters_.error_threshold,
                          /*search_range=*/2 *
                              mapping_parameters_.max_insert_size,
                          index, paired_end_mapping_metadata);
                  current_num_candidates1 =
                      paired_end_mapping_metadata.mapping_metadata1_
                          .GetNumCandidates();
                  current_num_candidates2 =
                      paired_end_mapping_metadata.mapping_metadata2_
                          .GetNumCandidates();
                }

                if (current_num_candidates1 > 0 &&
                    current_num_candidates2 > 0 &&
                    !mapping_parameters_.split_alignment) {
                  paired_end_mapping_metadata.MoveCandidiatesToBuffer();

                  // Paired-end filter
                  candidate_processor.ReduceCandidatesForPairedEndRead(
                      mapping_parameters_.max_insert_size,
                      paired_end_mapping_metadata);

                  current_num_candidates1 =
                      paired_end_mapping_metadata.mapping_metadata1_
                          .GetNumCandidates();
                  current_num_candidates2 =
                      paired_end_mapping_metadata.mapping_metadata2_
                          .GetNumCandidates();
                }

                // Verify candidates
                if (current_num_candidates1 > 0 &&
                    current_num_candidates2 > 0) {
                  thread_num_candidates +=
                      current_num_candidates1 + current_num_candidates2;

                  if (mapping_parameters_.custom_rid_order_file_path.length() >
                      0) {
                    RerankCandidatesRid(
                        paired_end_mapping_metadata.mapping_metadata1_
                            .positive_candidates_);
                    RerankCandidatesRid(
                        paired_end_mapping_metadata.mapping_metadata1_
                            .negative_candidates_);
                    RerankCandidatesRid(
                        paired_end_mapping_metadata.mapping_metadata2_
                            .positive_candidates_);
                    RerankCandidatesRid(
                        paired_end_mapping_metadata.mapping_metadata2_
                            .negative_candidates_);
                  }

                  draft_mapping_generator.GenerateDraftMappings(
                      read_batch1, pair_index, reference,
                      paired_end_mapping_metadata.mapping_metadata1_);

                  const size_t current_num_draft_mappings1 =
                      paired_end_mapping_metadata.mapping_metadata1_
                          .GetNumDraftMappings();

                  draft_mapping_generator.GenerateDraftMappings(
                      read_batch2, pair_index, reference,
                      paired_end_mapping_metadata.mapping_metadata2_);

                  const size_t current_num_draft_mappings2 =
                      paired_end_mapping_metadata.mapping_metadata2_
                          .GetNumDraftMappings();

                  if (current_num_draft_mappings1 > 0 &&
                      current_num_draft_mappings2 > 0) {
                    std::vector<std::vector<MappingRecord>>
                        &mappings_on_diff_ref_seqs =
                            mappings_on_diff_ref_seqs_for_diff_threads
                                [omp_get_thread_num()];

                    if (!mapping_parameters_.split_alignment) {
                      // GenerateBestMappingsForPairedEndRead assumes the
                      // mappings are sorted by coordinate for non split
                      // alignments. In split alignment, we don't want to sort
                      // and this keeps mapping and split_sites vectors
                      // consistent.
                      paired_end_mapping_metadata.SortMappingsByPositions();
                    }

                    int force_mapq = -1;
                    if (supplementCandidateResult != 0) {
                      force_mapq = 0;
                    }

                    mapping_generator.GenerateBestMappingsForPairedEndRead(
                        pair_index, read_batch1, read_batch2, barcode_batch,
                        reference, best_mapping_indices, generator, force_mapq,
                        paired_end_mapping_metadata, mappings_on_diff_ref_seqs);

                    if (paired_end_mapping_metadata.GetNumBestMappings() == 1) {
                      ++thread_num_uniquely_mapped_reads;
                      ++thread_num_uniquely_mapped_reads;
                    }

                    thread_num_mappings += std::min(
                        paired_end_mapping_metadata.GetNumBestMappings(),
                        mapping_parameters_.max_num_best_mappings);
                    thread_num_mappings += std::min(
                        paired_end_mapping_metadata.GetNumBestMappings(),
                        mapping_parameters_.max_num_best_mappings);
                    if (paired_end_mapping_metadata.GetNumBestMappings() > 0) {
                      ++thread_num_mapped_reads;
                      ++thread_num_mapped_reads;

                      if (read_map_summary != NULL)
                        read_map_summary[pair_index] |= (cache_miss < 2 ? 2 : 0) ;
                    }
                  }
                }  // verify candidate
              }
            } else {
              if (read_map_summary != NULL)
                read_map_summary[pair_index] = 0 ;
            }
          }  // end of for pair_index

          // if (num_reads_ / 2 > initial_num_sample_barcodes_) {
          //  if (!is_bulk_data_) {
          //    if (!barcode_whitelist_file_path_.empty()) {
          //      UpdateBarcodeAbundance(num_loaded_pairs, barcode_batch);
          //    }
          //  }
          //}
#pragma omp taskloop grainsize( std::max(history_update_threshold / mapping_parameters_.num_threads, (unsigned int)grain_size) )
          // Update cache
          for (uint32_t pair_index = 0; pair_index < history_update_threshold;
               ++pair_index) {
            if (mm_history1[pair_index].timestamp != num_reads_) continue;

            mm_to_candidates_cache.Update(
                mm_history1[pair_index].minimizers,
                mm_history1[pair_index].positive_candidates,
                mm_history1[pair_index].negative_candidates,
                mm_history1[pair_index].repetitive_seed_length,
                mapping_parameters_.debug_cache);
            mm_to_candidates_cache.Update(
                mm_history2[pair_index].minimizers,
                mm_history2[pair_index].positive_candidates,
                mm_history2[pair_index].negative_candidates,
                mm_history2[pair_index].repetitive_seed_length,
                mapping_parameters_.debug_cache);

            if (mm_history1[pair_index].positive_candidates.size() > 50) {
              std::vector<Candidate>().swap(
                  mm_history1[pair_index].positive_candidates);
            }
            if (mm_history1[pair_index].negative_candidates.size() > 50) {
              std::vector<Candidate>().swap(
                  mm_history1[pair_index].negative_candidates);
            }
            if (mm_history2[pair_index].positive_candidates.size() > 50) {
              std::vector<Candidate>().swap(
                  mm_history2[pair_index].positive_candidates);
            }
            if (mm_history2[pair_index].negative_candidates.size() > 50) {
              std::vector<Candidate>().swap(
                  mm_history2[pair_index].negative_candidates);
            }
          }

#pragma omp taskwait
          if (!mapping_parameters_.summary_metadata_file_path.empty()) {
            // Update total read count and number of cache hits
            if (mapping_parameters_.is_bulk_data) 
            {
              // Sum up cache hits for each thread
              int cache_hits_for_batch = 0;
              for (int hits: cache_hits_per_thread) {
                cache_hits_for_batch += hits;
              }
              mapping_writer.UpdateSummaryMetadata(0, 
                                                   SUMMARY_METADATA_TOTAL, 
                                                   num_loaded_pairs);
              mapping_writer.UpdateSummaryMetadata(0,
                                                   SUMMARY_METADATA_CACHEHIT, 
                                                   cache_hits_for_batch);
            }
            else 
            {
              for (uint32_t pair_index = 0; pair_index < num_loaded_pairs; ++pair_index) 
              {
                uint64_t pair_seed = seeds_for_batch[pair_index];
                if (read_map_summary[pair_index] & 1) 
                {
                  mapping_writer.UpdateSummaryMetadata(
                                            pair_seed, 
                                            SUMMARY_METADATA_TOTAL, 
                                            1);
                }
                if (read_map_summary[pair_index] & 2) 
                {
                  mapping_writer.UpdateSummaryMetadata( 
                                            pair_seed,
                                            SUMMARY_METADATA_CACHEHIT, 
                                            1);
                }
              }
            }            
            memset(read_map_summary, 1, sizeof(*read_map_summary)*read_batch_size_);
          }

          std::cerr << "Mapped " << num_loaded_pairs << " read pairs in "
                    << GetRealTime() - real_batch_start_time << "s.\n";
          real_batch_start_time = GetRealTime();

          // Swap to next batch
          num_loaded_pairs = num_loaded_pairs_for_loading;
          read_batch1_for_loading.SwapSequenceBatch(read_batch1);
          read_batch2_for_loading.SwapSequenceBatch(read_batch2);
          barcode_batch_for_loading.SwapSequenceBatch(barcode_batch);
          mappings_on_diff_ref_seqs_for_diff_threads.swap(
              mappings_on_diff_ref_seqs_for_diff_threads_for_saving);

          // Reset for next batch
          std::fill(seeds_for_batch.begin(), seeds_for_batch.end(), 0);

#pragma omp task
          {
            // Handle output
            num_mappings_in_mem +=
                mapping_processor.MoveMappingsInBuffersToMappingContainer(
                    num_reference_sequences,
                    mappings_on_diff_ref_seqs_for_diff_threads_for_saving,
                    mappings_on_diff_ref_seqs);
            if (mapping_parameters_.low_memory_mode &&
                num_mappings_in_mem > max_num_mappings_in_mem) {
              mapping_processor.ParallelSortOutputMappings(num_reference_sequences,
                                                   mappings_on_diff_ref_seqs, 0);

              mapping_writer.OutputTempMappings(num_reference_sequences,
                                                mappings_on_diff_ref_seqs,
                                                temp_mapping_file_handles);
              if (temp_mapping_file_handles.size() > 850
                  && temp_mapping_file_handles.size() % 10 == 1) { // every 10 temp files, double the temp file size
                max_num_mappings_in_mem <<= 1;
                std::cerr << "Used " << temp_mapping_file_handles.size() << "temp files. Double the temp file volume to " << max_num_mappings_in_mem << "\n" ;
              }
              num_mappings_in_mem = 0;
            }
          }  // end of omp task to handle output
        }    // end of while num_loaded_pairs
      }      // end of openmp single

      num_barcode_in_whitelist_ += thread_num_barcode_in_whitelist;
      num_corrected_barcode_ += thread_num_corrected_barcode;
      num_candidates_ += thread_num_candidates;
      num_mappings_ += thread_num_mappings;
      num_mapped_reads_ += thread_num_mapped_reads;
      num_uniquely_mapped_reads_ += thread_num_uniquely_mapped_reads;
    }  // end of openmp parallel region

    read_batch1_for_loading.FinalizeLoading();
    read_batch2_for_loading.FinalizeLoading();

    if (!mapping_parameters_.is_bulk_data) {
      barcode_batch_for_loading.FinalizeLoading();
    }
  }  // end of for read_file_index

  std::cerr << "Mapped all reads in " << GetRealTime() - real_start_mapping_time
            << "s.\n";

  delete[] mm_history1;
  delete[] mm_history2;
  if (read_map_summary != NULL)
    delete[] read_map_summary;

  OutputMappingStatistics();
  if (!mapping_parameters_.is_bulk_data) {
    OutputBarcodeStatistics();
  }

  index.Destroy();

  if (mapping_parameters_.low_memory_mode) {
    // First, process the remaining mappings in the memory and save them on
    // disk.
    if (num_mappings_in_mem > 0) {
      mapping_processor.SortOutputMappings(num_reference_sequences,
                                           mappings_on_diff_ref_seqs);

      mapping_writer.OutputTempMappings(num_reference_sequences,
                                        mappings_on_diff_ref_seqs,
                                        temp_mapping_file_handles);
      num_mappings_in_mem = 0;
    }

    mapping_writer.ProcessAndOutputMappingsInLowMemory(
        num_mappings_in_mem, num_reference_sequences, reference,
        barcode_whitelist_lookup_table_, temp_mapping_file_handles);
  } 
  else {
    if (mapping_parameters_.Tn5_shift) {
      mapping_processor.ApplyTn5ShiftOnMappings(num_reference_sequences,
                                                mappings_on_diff_ref_seqs);
    }

    if (mapping_parameters_.remove_pcr_duplicates) {
      mapping_processor.RemovePCRDuplicate(num_reference_sequences,
                                           mappings_on_diff_ref_seqs,
                                           mapping_parameters_.num_threads);
      std::cerr << "After removing PCR duplications, ";
      mapping_processor.OutputMappingStatistics(num_reference_sequences,
                                                mappings_on_diff_ref_seqs);
    } else {
      mapping_processor.ParallelSortOutputMappings(num_reference_sequences,
                                           mappings_on_diff_ref_seqs,
                                           mapping_parameters_.num_threads);
    }

    if (mapping_parameters_.allocate_multi_mappings) {
      const uint64_t num_multi_mappings =
          num_mapped_reads_ - num_uniquely_mapped_reads_;
      mapping_processor.AllocateMultiMappings(
          num_reference_sequences, num_multi_mappings,
          mapping_parameters_.multi_mapping_allocation_distance,
          mappings_on_diff_ref_seqs);
      std::cerr << "After allocating multi-mappings, ";
      mapping_processor.OutputMappingStatistics(num_reference_sequences,
                                                mappings_on_diff_ref_seqs);
      mapping_processor.SortOutputMappings(num_reference_sequences,
                                           mappings_on_diff_ref_seqs);
    }
    mapping_writer.OutputMappings(num_reference_sequences, reference,
                                  mappings_on_diff_ref_seqs);
    // Temporarily disable feature matrix output. Do not delete the following
    // commented code.
    // if (!is_bulk_data_ && !matrix_output_prefix_.empty()) {
    //   if constexpr (std::is_same<MappingRecord,
    //                             PairedEndMappingWithBarcode>::value) {
    //    FeatureBarcodeMatrix feature_barcode_matrix(
    //        cell_by_bin_, bin_size_, multi_mapping_allocation_distance_,
    //        depth_cutoff_to_call_peak_);
    //    std::vector<std::vector<PairedEndMappingWithBarcode>> &mappings =
    //        allocate_multi_mappings_
    //            ? allocated_mappings_on_diff_ref_seqs
    //            : (remove_pcr_duplicates_ ? deduped_mappings_on_diff_ref_seqs
    //                                      : mappings_on_diff_ref_seqs);

    //    feature_barcode_matrix.OutputFeatureMatrix(num_reference_sequences,
    //                                               reference, mappings,
    //                                               matrix_output_prefix_);
    //  }
    //}
  }
  
  if (mapping_parameters_.mapping_output_format == MAPPINGFORMAT_SAM)
    mapping_writer.AdjustSummaryPairedEndOverCount() ;

  // Destory the locks used for map
  for (int i = 0; i < num_locks_for_map; ++i) {
    omp_destroy_lock(&map_locks[i]);
  }

  // Add cardinality information to summary metadata
  if (output_num_cache_slots_info) {
    for (auto curr_map: barcode_peak_map) {
      for (auto &pair: curr_map) {
        size_t curr_seed = pair.first;
        size_t est_num_slots = pair.second.compute_cardinality();

        mapping_writer.UpdateSummaryMetadata( 
                          curr_seed,
                          SUMMARY_METADATA_CARDINALITY, 
                          est_num_slots);
      }
    }
  }

  mapping_writer.OutputSummaryMetadata(frip_est_params, output_num_cache_slots_info);
  reference.FinalizeLoading();
  if (mapping_parameters_.debug_cache) {mm_to_candidates_cache.PrintStats();}
  
  std::cerr << "Total time: " << GetRealTime() - real_start_time << "s.\n";
}

}  // namespace chromap

#endif  // CHROMAP_H_
