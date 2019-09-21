/* Copyright 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_BRAVE_PERF_PREDICTOR_BROWSER_BANDWIDTH_LINREG_PARAMETERS_H_
#define BRAVE_COMPONENTS_BRAVE_PERF_PREDICTOR_BROWSER_BANDWIDTH_LINREG_PARAMETERS_H_

/* This file is automatically generated, do not edit directly */

#include <string>
#include <array>

#include "base/containers/flat_set.h"
#include "base/containers/flat_map.h"

namespace brave_perf_predictor {

constexpr double model_intercept = {{model.intercept}};
constexpr int feature_count = {{model.coefficients | length}};
constexpr std::array<double, feature_count> model_coefficients = {
{{model.coefficients | join(',\n')}}
};

constexpr unsigned int standardise_feat_count = {{transformers.standardise.features | length}};

constexpr std::array<double, standardise_feat_count> standardise_feat_means = {
{{transformers.standardise.mean | join(',\n')}}
};

constexpr std::array<double, standardise_feat_count> standardise_feat_scale = {
{{transformers.standardise.scale | join(',\n')}}
};

const std::array<std::string, feature_count> feature_sequence{
    {% for feature in transformers.standardise.features %}
    "{{feature}}",
    {% endfor %}
    {% for feature in transformers.passthrough.features %}
    "{{feature}}",
    {% endfor %}
};

const std::array<std::string, {{misc.entities | length}}> relevant_entities{
  {% for entity in misc.entities %}
  "{{entity}}",
  {% endfor %}
};

const base::flat_set<std::string> relevant_entity_set(
    relevant_entities.begin(),
    relevant_entities.end());

struct stdfactor {
  double mean, scale;
};

const base::flat_map<std::string, stdfactor> stdfactor_map = {
  {% for feature, (mean, scale) in transformers.standardise.feature_map %}
  {
    "{{feature}}",
    { {{mean}}, {{scale}} }
  },
  {% endfor %}
};

}  // namespace brave_perf_predictor

#endif  // BRAVE_COMPONENTS_BRAVE_PERF_PREDICTOR_BROWSER_BANDWIDTH_LINREG_PARAMETERS_H_
