#include "../generators.h"
#include "model.h"
#include "kv_cache.h"

namespace Generators {

KV_Cache_Combined::KV_Cache_Combined(Model& model, const SearchParams& search_params)
    : model_{model},
      layer_count_{model.config_->num_hidden_layers},
      shape_{2, search_params.batch_size * search_params.num_beams, model.config_->num_attention_heads, 0, model.config_->hidden_size},
      empty_past_{OrtValue::CreateTensor(*model_.allocator_device_, shape_, model_.score_type_)} {
  pasts_.resize(layer_count_);
  presents_.reserve(layer_count_);

  shape_[3] = search_params.sequence_length;
  for (int i = 0; i < layer_count_; ++i) {
    presents_.push_back(OrtValue::CreateTensor(*model.allocator_device_, shape_, model_.score_type_));

    char string[32];
    snprintf(string, std::size(string), past_name_, i);
    input_name_strings_.push_back(string);

    snprintf(string, std::size(string), present_name_, i);
    output_name_strings_.push_back(string);
  }
}

void KV_Cache_Combined::Update(std::span<const int32_t> beam_indices, int current_length) {
  for (int i = 0; i < layer_count_; i++) {
    if (beam_indices.empty())
      pasts_[i] = std::move(presents_[i]);
    else
      PickPastState(beam_indices, i);
  }

  shape_[3] = current_length;
  for (int i = 0; i < layer_count_; i++)
    presents_[i] = OrtValue::CreateTensor(*model_.allocator_device_, shape_, model_.score_type_);
}

// Copy present state to past state reordered by the beam_indices
template <typename ScoreType>
void KV_Cache_Combined::PickPastState(std::span<const int32_t> beam_indices, int index) {
  auto block_size_per_beam = shape_[2] * shape_[3] * shape_[4];
  auto past_key_size = shape_[1] * block_size_per_beam;
  auto element_count = shape_[0] * past_key_size;

  const OrtValue& present = *presents_[index];
  auto past = OrtValue::CreateTensor<ScoreType>(*model_.allocator_device_, shape_);
  auto past_span = std::span<ScoreType>(past->GetTensorMutableData<ScoreType>(), element_count);
  auto present_span = std::span<const ScoreType>(present.GetTensorData<ScoreType>(), element_count);

#if USE_CUDA
  if (model_.device_type_==DeviceType::CUDA) {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present_key = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto present_value = present_span.subspan(past_key_size + beam_index * block_size_per_beam, block_size_per_beam);

      auto past_key = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      auto past_value = past_span.subspan(past_key_size + j * block_size_per_beam, block_size_per_beam);
      cudaMemcpyAsync(past_key.data(), present_key.data(), present_key.size_bytes(), cudaMemcpyDeviceToDevice, model_.cuda_stream_);
      cudaMemcpyAsync(past_value.data(), present_value.data(), present_value.size_bytes(), cudaMemcpyDeviceToDevice, model_.cuda_stream_);
    }
  } else
#endif
  {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present_key = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto present_value = present_span.subspan(past_key_size + beam_index * block_size_per_beam, block_size_per_beam);

      auto past_key = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      auto past_value = past_span.subspan(past_key_size + j * block_size_per_beam, block_size_per_beam);
      copy(present_key, past_key);
      copy(present_value, past_value);
    }
  }

  pasts_[index] = std::move(past);
}

void KV_Cache_Combined::PickPastState(std::span<const int32_t> beam_indices, int index) {
  if (model_.score_type_ == Ort::TypeToTensorType<float>::type)
    PickPastState<float>(beam_indices, index);
  else
    PickPastState<Ort::Float16_t>(beam_indices, index);
}

KV_Cache::KV_Cache(Model& model, const SearchParams& search_params,
                   std::span<const char*> past_names, std::span<const char*> present_names, std::span<const char*> past_cross_names, std::span<const char*> present_cross_names)
    : model_{model},
      layer_count_{model.config_->num_hidden_layers},
      past_names_{past_names},
      present_names_{present_names},
      past_cross_names_{past_cross_names},
      present_cross_names_{present_cross_names},
      shape_{search_params.batch_size * search_params.num_beams, model.config_->num_attention_heads, 0, model.config_->hidden_size},
      cross_shape_{search_params.batch_size * search_params.num_beams, model.config_->num_attention_heads, 1500, model.config_->hidden_size},
      empty_past_{OrtValue::CreateTensor(*model_.allocator_device_, shape_, model_.score_type_)} {
  pasts_.resize(layer_count_ * 2);
  presents_.reserve(layer_count_ * 2);

  shape_[2] = search_params.sequence_length;

  for (int i = 0; i < layer_count_; ++i) {
    presents_.push_back(OrtValue::CreateTensor(*model_.allocator_device_, shape_, model_.score_type_));
    presents_.push_back(OrtValue::CreateTensor(*model_.allocator_device_, shape_, model_.score_type_));

    char string[32];
    for(auto *name : past_names_) {
      snprintf(string, std::size(string), name, i);
      input_name_strings_.push_back(string);
    }
    for (auto* name : present_names_) {
      snprintf(string, std::size(string), name, i);
      output_name_strings_.push_back(string);
    }
    if (!past_cross_names_.empty()) {
      crosses_.push_back(OrtValue::CreateTensor(*model_.allocator_device_, cross_shape_, model_.score_type_));
      crosses_.push_back(OrtValue::CreateTensor(*model_.allocator_device_, cross_shape_, model_.score_type_));

      for (auto* name : past_cross_names_) {
        snprintf(string, std::size(string), name, i);
        input_cross_name_strings_.push_back(string);
      }
      for (auto* name : present_cross_names_) {
        snprintf(string, std::size(string), name, i);
        output_cross_name_strings_.push_back(string);
      }
    }
  }
}

void KV_Cache::Update(std::span<const int32_t> beam_indices, int current_length) {
  for (int i = 0; i < layer_count_ * 2; i++) {
    if (beam_indices.empty())
      pasts_[i] = std::move(presents_[i]);
    else
      PickPastState(beam_indices, i);
  }

  shape_[2] = current_length;
  for (int i = 0; i < layer_count_ * 2; i++)
    presents_[i] = OrtValue::CreateTensor(*model_.allocator_device_, shape_, model_.score_type_);
}

// Copy present state to past state reordered by the beam_indices
template <typename ScoreType>
void KV_Cache::PickPastState(std::span<const int32_t> beam_indices, int index) {
  auto block_size_per_beam = shape_[1] * shape_[2] * shape_[3];
  auto element_count = shape_[0] * block_size_per_beam;

  const OrtValue& present = *presents_[index];
  auto past = OrtValue::CreateTensor<ScoreType>(*model_.allocator_device_, shape_);
  auto past_span = std::span<ScoreType>(past->GetTensorMutableData<ScoreType>(), element_count);
  auto present_span = std::span<const ScoreType>(present.GetTensorData<ScoreType>(), element_count);

#if USE_CUDA
  if (model_.device_type_==DeviceType::CUDA) {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto past = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      cudaMemcpyAsync(past.data(), present.data(), present.size_bytes(), cudaMemcpyDeviceToDevice, model_.cuda_stream_);
    }
  } else
#endif
  {
    for (size_t j = 0; j < beam_indices.size(); j++) {
      int32_t beam_index = beam_indices[j];
      auto present = present_span.subspan(beam_index * block_size_per_beam, block_size_per_beam);
      auto past = past_span.subspan(j * block_size_per_beam, block_size_per_beam);
      copy(present, past);
    }
  }

  pasts_[index] = std::move(past);
}

void KV_Cache::PickPastState(std::span<const int32_t> beam_indices, int index) {
  if (model_.score_type_ == Ort::TypeToTensorType<float>::type)
    PickPastState<float>(beam_indices, index);
  else
    PickPastState<Ort::Float16_t>(beam_indices, index);
}

}  // namespace Generators
