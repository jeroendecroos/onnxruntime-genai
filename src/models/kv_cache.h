#pragma once

namespace Generators {

struct KV_Cache_Combined {

  KV_Cache_Combined(Model& model, const SearchParams& search_params);
  void Update(std::span<const int32_t> beam_indices, int current_length);
  template <typename ScoreType>
  void PickPastState(std::span<const int32_t> beam_indices, int index);
  void PickPastState(std::span<const int32_t> beam_indices, int index);

  // KV combined
  const char *past_name_{"past_%d"};
  const char *present_name_{"present_%d"};

  Model& model_;
  int layer_count_;

  std::array<int64_t, 5> shape_;

  std::unique_ptr<OrtValue> empty_past_;
  std::vector<std::unique_ptr<OrtValue>> pasts_, presents_;
  std::vector<std::string> input_name_strings_, output_name_strings_;
};

struct KV_Cache {
  KV_Cache(Model& model, const SearchParams& search_params, std::span<const char*> past_names, std::span<const char*> present_names, std::span<const char*> past_cross_names = {}, std::span<const char*> present_cross_names = {});
  void Update(std::span<const int32_t> beam_indices, int current_length);
  template <typename ScoreType>
  void PickPastState(std::span<const int32_t> beam_indices, int index);
  void PickPastState(std::span<const int32_t> beam_indices, int index);

  Model& model_;
  int layer_count_;

  std::span<const char*> past_names_;     // past key name/past value name
  std::span<const char*> present_names_;  // present key name/present value name
  std::span<const char*> past_cross_names_, present_cross_names_;

  std::array<int64_t, 4> shape_, cross_shape_;

  std::unique_ptr<OrtValue> empty_past_;
  std::vector<std::unique_ptr<OrtValue>> pasts_, presents_, crosses_;
  std::vector<std::string> input_name_strings_, output_name_strings_;
  std::vector<std::string> input_cross_name_strings_, output_cross_name_strings_;
};

} // namespace Generators
