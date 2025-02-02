//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#ifndef ZETASQL_RESOLVED_AST_RESOLVED_COLLATION_H_
#define ZETASQL_RESOLVED_AST_RESOLVED_COLLATION_H_

#include "zetasql/public/types/annotation.h"
#include "zetasql/resolved_ast/serialization.pb.h"

namespace zetasql {

// This class is used with zetasql::Type to indicate the resolved collation
// for the type. For nested types, see comments on <child_list_> for how
// collation on subfield(s) are stored.
// This is always stored in a normalized form, meaning on all the nested levels,
// it has either an empty <child_list_> to indicate that it has no collation in
// any child, or it has at least one non-empty child.
class ResolvedCollation {
 public:
  // Iterates the <annotation_map> and makes a normalized ResolvedCollation
  // instance.
  static zetasql_base::StatusOr<ResolvedCollation> MakeResolvedCollation(
      const AnnotationMap& annotation_map);

  // Constructs empty ResolvedCollation. Default constructor must be public to
  // be used in the ResolvedAST.
  ResolvedCollation() = default;

  // ResolvedCollation is movable.
  ResolvedCollation(ResolvedCollation&&) = default;
  ResolvedCollation& operator=(ResolvedCollation&& that) = default;

  // When copied, the underlying string in <collation_name_> is shared. See
  // zetasql::SimpleValue for more detail.
  ResolvedCollation(const ResolvedCollation&) = default;
  ResolvedCollation& operator=(const ResolvedCollation& that) = default;

  // Returns true if current type has no collation and has no children with
  // collation.
  bool Empty() const {
    return !collation_name_.IsValid() && child_list_.empty();
  }

  bool Equals(const ResolvedCollation& that) const;
  bool operator==(const ResolvedCollation& that) const { return Equals(that); }

  // Collation on current type (STRING), not on subfields.
  bool HasCollation() const {
    return collation_name_.has_string_value() &&
           !collation_name_.string_value().empty();
  }

  absl::string_view CollationName() const {
    static char kEmptyCollation[] = "";
    return collation_name_.has_string_value()
               ? collation_name_.string_value()
               : absl::string_view(kEmptyCollation);
  }

  // Children only exist if any of the children have a collation. See comments
  // on <child_list_> for more detail.
  const std::vector<ResolvedCollation>& child_list() const {
    return child_list_;
  }
  const ResolvedCollation& child(int i) const { return child_list_[i]; }
  uint64_t num_children() const { return child_list_.size(); }

  absl::Status Serialize(ResolvedCollationProto* proto) const;
  static zetasql_base::StatusOr<ResolvedCollation> Deserialize(
      const ResolvedCollationProto& proto);

 private:
  // Stores ResolvedCollation for subfields for ARRAY/STRUCT types.
  // <child_list_> could be empty to indicate that the ARRAY/STRUCT doesn't have
  // collation in subfield(s).
  // When <child_list_> is not empty, for ARRAY, the size of <child_list_>
  // must be 1; for STRUCT, the size of <child_list_> must be the same as the
  // number of the fields the STRUCT has.
  std::vector<ResolvedCollation> child_list_;

  // This SimpleValue instance is either TYPE_INVALID to indicate there is no
  // collation or TYPE_STRING to store the collation name.
  SimpleValue collation_name_;
};

}  // namespace zetasql

#endif  // ZETASQL_RESOLVED_AST_RESOLVED_COLLATION_H_
