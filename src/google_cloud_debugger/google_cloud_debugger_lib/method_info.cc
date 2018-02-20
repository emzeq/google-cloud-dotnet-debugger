// Copyright 2018 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "method_info.h"

#include <iostream>
#include <queue>
#include <vector>

#include "dbg_stack_frame.h"
#include "i_cor_debug_helper.h"

/*
#include "compiler_helpers.h"
#include "constants.h"
#include "dbg_breakpoint.h"
#include "dbg_class_property.h"
#include "debugger_callback.h"
#include "i_cor_debug_helper.h"
#include "i_eval_coordinator.h"
#include "type_signature.h"
#include "variable_wrapper.h"
*/

namespace google_cloud_debugger {

HRESULT MethodInfo::PopulateMethodDefFromNameAndArguments(
    IMetaDataImport *metadata_import, const mdTypeDef &class_token,
    DbgStackFrame *stack_frame) {
  if (metadata_import == nullptr) {
    return E_INVALIDARG;
  }

  std::vector<mdMethodDef> method_defs;
  HRESULT hr = GetMethodDefsFromName(metadata_import, class_token, method_name,
                                     &method_defs, &std::cerr);
  if (FAILED(hr)) {
    return hr;
  }

  if (method_defs.empty()) {
    return E_FAIL;
  }

  for (const mdMethodDef &method_def : method_defs) {
    mdTypeDef class_type;
    ULONG method_name_len;
    DWORD method_attribute;
    PCCOR_SIGNATURE method_sig;
    ULONG method_sig_len;
    ULONG method_rva;
    DWORD method_impl_flags;
    hr = metadata_import->GetMethodProps(method_def, &class_type, nullptr, 0,
                                         &method_name_len, &method_attribute,
                                         &method_sig, &method_sig_len,
                                         &method_rva, &method_impl_flags);
    if (FAILED(hr)) {
      continue;
    }

    ULONG bytes_read;
    ULONG calling_convention;
    hr = CorSigUncompressData(method_sig, method_sig_len, &calling_convention,
                              &bytes_read);
    if (FAILED(hr)) {
      return hr;
    }

    if (bytes_read != 1) {
      return META_E_BAD_SIGNATURE;
    }

    method_sig_len -= bytes_read;
    // If generic method, the next bytes would be the number of generic
    // parameters.
    if (calling_convention &
        CorCallingConvention::IMAGE_CEE_CS_CALLCONV_GENERIC != 0) {
      ULONG generic_param_count = 0;
      hr = CorSigUncompressData(method_sig, method_sig_len,
                                &generic_param_count, &bytes_read);
      if (FAILED(hr)) {
        return hr;
      }
      method_sig_len -= bytes_read;
    }

    // Now we get the number of parameters in the signature.
    ULONG param_count = 0;
    hr = CorSigUncompressData(method_sig, method_sig_len, &param_count,
                              &bytes_read);
    if (FAILED(hr)) {
      return hr;
    }

    // This is true if there is an explicit this parameter.
    bool explicitThis =
        calling_convention &
        CorCallingConvention::IMAGE_CEE_CS_CALLCONV_EXPLICITTHIS != 0;
    if (explicitThis) {
      param_count -= 1;
    }

    method_sig_len -= bytes_read;
    // The param count has to match. Otherwise, this is not
    // the method we are looking for.
    if (param_count != argument_types.size()) {
      continue;
    }

    // Now we can extract the return type.
    std::string returned_type;
    hr = ParseTypeFromSig(method_sig, &method_sig_len, metadata_import,
                          &returned_type);
    if (FAILED(hr)) {
      return hr;
    }

    // If there is an explicit this, there will be a this parameter.
    if (calling_convention &
        CorCallingConvention::IMAGE_CEE_CS_CALLCONV_EXPLICITTHIS != 0) {
      // Extracts out this "this" parameter.
      std::string this_type;
      hr = ParseTypeFromSig(method_sig, &method_sig_len, metadata_import,
                            &returned_type);
      if (FAILED(hr)) {
        return hr;
      }
      --param_count;
    }

    bool matched_method = true;
    // Now we extracts the parameters and compares their types with arguments'
    // types.
    for (size_t i = 0; i < param_count; ++i) {
      std::string parameter_type;
      hr = ParseTypeFromSig(method_sig, &method_sig_len, metadata_import,
                            &parameter_type);
      if (FAILED(hr)) {
        return hr;
      }

      // Moves on if we can't find matching parameters.
      // Matching here means the parameter type is the same as the
      // type of the actual or if the argument type is a child class
      // of the parameter type.
      if (argument_types[i].compare(parameter_type) != 0 ||
          !stack_frame->IsBaseType(argument_types[i], parameter_type,
                                   &std::cerr)) {
        matched_method = false;
        break;
      }
    }

    if (matched_method) {
      // Now we match the params type with arguments type.
      is_static = IsMdStatic(method_attribute);
      method_token = method_def;
      // Checks the generic types.
      uint32_t method_generic_types = 0;
      hr = CountGenericParams(metadata_import, method_def,
                              &method_generic_types);
      if (FAILED(hr)) {
        return hr;
      }
      has_generic_types = method_generic_types != 0;

      return S_OK;
    }
  }

  return S_FALSE;
}

HRESULT MethodInfo::GetMethodDefsFromName(
    IMetaDataImport *metadata_import, const mdTypeDef &class_token,
    std::vector<mdMethodDef> *methods_matched, std::ostream *err_stream) {
  if (methods_matched == nullptr) {
    return E_INVALIDARG;
  }

  HCORENUM cor_enum = nullptr;
  std::vector<WCHAR> wchar_method_name = ConvertStringToWCharPtr(method_name);
  std::vector<mdMethodDef> method_defs(100, 0);
  HRESULT hr = S_OK;

  while (hr == S_OK) {
    ULONG method_defs_returned = 0;
    hr = metadata_import->EnumMethodsWithName(
        &cor_enum, class_token, wchar_method_name.data(), method_defs.data(),
        method_defs.size(), &method_defs_returned);
    if (FAILED(hr)) {
      *err_stream << "Failed to enumerate method name " << method_name;
      break;
    }

    if (method_defs_returned == 0) {
      break;
    }
    method_defs.resize(method_defs_returned);
    methods_matched->insert(methods_matched->end(), method_defs.begin(),
                            method_defs.end());
  }

  metadata_import->CloseEnum(cor_enum);
  return S_OK;
}

}  //  namespace google_cloud_debugger
