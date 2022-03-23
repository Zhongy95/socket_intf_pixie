/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <utility>

#include "src/carnot/planner/ir/ir.h"
#include "src/carnot/planner/ir/otel_export_sink_ir.h"
#include "src/carnot/planpb/plan.pb.h"

namespace px {
namespace carnot {
namespace planner {
std::string ConvertSemanticTypeToOtel(const types::SemanticType& stype) {
  switch (stype) {
      // Future writers should look through to find your metric's standards.
      // Most semantic types will not have one.
      // https://ucum.org/ucum.html#section-Tables-of-Terminal-Symbols
    case types::ST_DURATION_NS:
    case types::ST_DURATION_NS_QUANTILES:
      return "ns";
    case types::ST_BYTES:
      return "By";
    case types::ST_PERCENT:
    case types::ST_THROUGHPUT_PER_NS:
      return "/ns";
    case types::ST_THROUGHPUT_BYTES_PER_NS:
      return "By/ns";
      // The following semantic types don't show up in the standards tables
      // for unit names. They shouldn't be used as metric values.

    case types::SemanticType_INT_MIN_SENTINEL_DO_NOT_USE_:
    case types::SemanticType_INT_MAX_SENTINEL_DO_NOT_USE_:
    case types::ST_UNSPECIFIED:
    case types::ST_NONE:
    case types::ST_TIME_NS:
    case types::ST_AGENT_UID:
    case types::ST_ASID:
    case types::ST_UPID:
    case types::ST_SERVICE_NAME:
    case types::ST_POD_NAME:
    case types::ST_POD_PHASE:
    case types::ST_POD_STATUS:
    case types::ST_NODE_NAME:
    case types::ST_CONTAINER_NAME:
    case types::ST_CONTAINER_STATE:
    case types::ST_CONTAINER_STATUS:
    case types::ST_NAMESPACE_NAME:
    case types::ST_QUANTILES:
    case types::ST_IP_ADDRESS:
    case types::ST_PORT:
    case types::ST_HTTP_REQ_METHOD:
    case types::ST_HTTP_RESP_STATUS:
    case types::ST_HTTP_RESP_MESSAGE:
    case types::ST_SCRIPT_REFERENCE:
      return "";
  }
  return "";
}

StatusOr<std::vector<absl::flat_hash_set<std::string>>> OTelExportSinkIR::RequiredInputColumns()
    const {
  return std::vector<absl::flat_hash_set<std::string>>{required_column_names_};
}

Status OTelExportSinkIR::ProcessConfig(

    const OTelData& data) {
  data_.endpoint_config = data.endpoint_config;
  for (const auto& attr : data.resource_attributes) {
    PL_ASSIGN_OR_RETURN(auto column, AddColumn(attr.column_reference));
    data_.resource_attributes.push_back({attr.name, column});
  }

  for (const auto& metric : data.metrics) {
    OTelMetric new_metric;
    new_metric.name = metric.name;
    new_metric.description = metric.description;

    PL_ASSIGN_OR_RETURN(new_metric.unit_column, AddColumn(metric.unit_column));
    PL_ASSIGN_OR_RETURN(new_metric.time_column, AddColumn(metric.time_column));
    for (const auto& attr : metric.attributes) {
      PL_ASSIGN_OR_RETURN(auto column, AddColumn(attr.column_reference));
      new_metric.attributes.push_back({attr.name, column});
    }

    PL_RETURN_IF_ERROR(std::visit(
        overloaded{
            [&new_metric, this](const OTelMetricGauge& gauge) {
              PL_ASSIGN_OR_RETURN(auto val, AddColumn(gauge.value_column));
              new_metric.metric = OTelMetricGauge{val};
              return Status::OK();
            },
            [&new_metric, this](const OTelMetricSummary& summary) {
              OTelMetricSummary new_summary;
              PL_ASSIGN_OR_RETURN(new_summary.count_column, AddColumn(summary.count_column));
              PL_ASSIGN_OR_RETURN(new_summary.sum_column, AddColumn(summary.sum_column));

              for (const auto& quantile : summary.quantiles) {
                PL_ASSIGN_OR_RETURN(auto column, AddColumn(quantile.value_column));
                new_summary.quantiles.push_back({quantile.quantile, column});
              }
              new_metric.metric = std::move(new_summary);
              return Status::OK();
            },
        },
        metric.metric));

    data_.metrics.push_back(std::move(new_metric));
  }
  for (const auto& span : data.spans) {
    // TODO(philkuz, PP3275) implement SPANS.
    PL_UNUSED(span);
  }
  return Status::OK();
}

Status OTelExportSinkIR::ToProto(planpb::Operator* op) const {
  op->set_op_type(planpb::OTEL_EXPORT_SINK_OPERATOR);
  auto otel_op = op->mutable_otel_sink_op();
  *otel_op->mutable_endpoint_config() = data_.endpoint_config;
  auto resource = otel_op->mutable_resource();
  for (const auto& otel_attribute : data_.resource_attributes) {
    PL_RETURN_IF_ERROR(otel_attribute.ToProto(resource->add_attributes()));
  }

  for (const auto& metric : data_.metrics) {
    auto metric_pb = otel_op->add_metrics();
    metric_pb->set_name(metric.name);
    metric_pb->set_description(metric.description);

    auto unit_type = static_cast<ValueType*>(metric.unit_column->resolved_type().get());
    metric_pb->set_unit(ConvertSemanticTypeToOtel(unit_type->semantic_type()));
    if (metric.time_column->EvaluatedDataType() != types::TIME64NS) {
      return metric.time_column->CreateIRNodeError(
          "Expected time column '$0' to be TIME64NS, received $1", metric.time_column->col_name(),
          types::ToString(metric.time_column->EvaluatedDataType()));
    }
    PL_ASSIGN_OR_RETURN(auto time_index, metric.time_column->GetColumnIndex());
    metric_pb->set_time_column_index(time_index);
    for (const auto& attribute : metric.attributes) {
      PL_RETURN_IF_ERROR(attribute.ToProto(metric_pb->add_attributes()));
    }

    PL_RETURN_IF_ERROR(std::visit(
        overloaded{
            [&metric_pb](const OTelMetricGauge& gauge) {
              auto gauge_pb = metric_pb->mutable_gauge();
              PL_ASSIGN_OR_RETURN(auto gauge_index, gauge.value_column->GetColumnIndex());
              switch (gauge.value_column->EvaluatedDataType()) {
                case types::INT64:
                  gauge_pb->set_int_column_index(gauge_index);
                  break;
                case types::FLOAT64:
                  gauge_pb->set_float_column_index(gauge_index);
                  break;
                default:
                  return gauge.value_column->CreateIRNodeError(
                      "Expected value column '$0' to be INT64 or FLOAT64, received $1",
                      gauge.value_column->col_name(),
                      types::ToString(gauge.value_column->EvaluatedDataType()));
              }
              return Status::OK();
            },
            [&metric_pb](const OTelMetricSummary& summary) {
              auto summary_pb = metric_pb->mutable_summary();
              PL_ASSIGN_OR_RETURN(auto count_index, summary.count_column->GetColumnIndex());
              if (summary.count_column->EvaluatedDataType() != types::INT64) {
                return summary.count_column->CreateIRNodeError(
                    "Expected count column '$0' to be INT64, received $1",
                    summary.count_column->col_name(),
                    types::ToString(summary.count_column->EvaluatedDataType()));
              }
              summary_pb->set_count_column_index(count_index);

              PL_ASSIGN_OR_RETURN(auto sum_index, summary.sum_column->GetColumnIndex());
              if (summary.sum_column->EvaluatedDataType() != types::FLOAT64) {
                return summary.sum_column->CreateIRNodeError(
                    "Expected sum column '$0' to be FLOAT64, received $1",
                    summary.sum_column->col_name(),
                    types::ToString(summary.sum_column->EvaluatedDataType()));
              }
              summary_pb->set_sum_column_index(sum_index);

              for (const auto& quantile : summary.quantiles) {
                if (quantile.value_column->EvaluatedDataType() != types::FLOAT64) {
                  return quantile.value_column->CreateIRNodeError(
                      "Expected quantile column '$0' to be FLOAT64, received $1",
                      quantile.value_column->col_name(),
                      types::ToString(quantile.value_column->EvaluatedDataType()));
                }
                PL_ASSIGN_OR_RETURN(auto value_column_index,
                                    quantile.value_column->GetColumnIndex());

                auto quantile_value_pb = summary_pb->add_quantile_values();
                quantile_value_pb->set_quantile(quantile.quantile);
                quantile_value_pb->set_value_column_index(value_column_index);
              }
              return Status::OK();
            },
        },
        metric.metric));
  }
  for (const auto& span : data_.spans) {
    // TODO(philkuz, PP3275) implement spans.
    PL_UNUSED(span);
  }
  return Status::OK();
}

Status OTelExportSinkIR::CopyFromNodeImpl(const IRNode* node,
                                          absl::flat_hash_map<const IRNode*, IRNode*>*) {
  const OTelExportSinkIR* source = static_cast<const OTelExportSinkIR*>(node);
  return ProcessConfig(source->data_);
}

Status OTelExportSinkIR::ResolveType(CompilerState* compiler_state) {
  DCHECK_EQ(1, parent_types().size());

  auto parent_table_type = std::static_pointer_cast<TableType>(parent_types()[0]);
  auto table = TableType::Create();
  for (const auto& column : columns_to_resolve_) {
    PL_RETURN_IF_ERROR(ResolveExpressionType(column, compiler_state, parent_types()));
    if (table->HasColumn(column->col_name())) {
      continue;
    }
    table->AddColumn(column->col_name(), column->resolved_type());
  }
  return SetResolvedType(table);
}

}  // namespace planner
}  // namespace carnot
}  // namespace px
