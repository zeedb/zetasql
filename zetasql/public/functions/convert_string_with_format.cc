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

#include "zetasql/public/functions/convert_string_with_format.h"

#include <vector>

#include "zetasql/public/functions/format_max_output_width.h"
#include "zetasql/public/functions/string_format.h"
#include "zetasql/public/functions/util.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "absl/strings/substitute.h"
#include "re2/re2.h"
#include "zetasql/base/status_macros.h"

// Format a numeric type value into a string with a format sting.
// For the spec, see (broken link).

namespace zetasql {
namespace functions {
namespace internal {
namespace {

// This class is used to parse format strings as defined in
// (broken link). It is implemented as a state machine.
class FormatParser {
 public:
  // Parses the format string and returns <parsed_format_element_info>.
  zetasql_base::StatusOr<ParsedFormatElementInfo> Parse(absl::string_view format);

 private:
  // There are 4 types of format strings:
  // 1. Text Minimal, e.g. "TM", "TM9"
  // 2. Roman numeral, e.g. "RN", "RNFM"
  // 3. Hexadecimal, e.g. "00X0X0"
  // 4. Decimal, e.g. "9.999"
  //
  // The format of text minimal and roman numeral format strings are pretty
  // simple.
  //
  // A hexadecimal format string has this format:
  //
  //   integer_part
  //
  // where "0" and "X" are allowed in the integer_part.
  //
  // A decimal format string has this format:
  //
  //   front_sign integer_part decimal_point fractional_part exponent back_sign
  //
  // where
  //   - "S" is allowed in front_sign.
  //   - "0", "9" and group separators are allowed in the integer_part.
  //   - decimal point is one of ".", "D", or "V". "." generates a "." in
  //     the output. "D" generates a decimal point in the current locale. "V" is
  //     treated as an invisible decimal point. It does not generate anything in
  //     the output. E.g. for input 1.2,
  //     - format string "9.99" generates " 1.20";
  //     - format string "9D99" generates " 1.20";
  //     - format string "9V99" generates " 120";
  //   - "0" and "9" are allowed in the fractional_part.
  //   - "EEEE" is allowed in exponent.
  //   - "S", "MI" and "PR" are allowed in back_sign.
  //
  // For example, for the format string "S9,9.00EEEEMI", it can be broken down
  // into:
  // - front_sign = "S"
  // - integer_part = "9,9"
  // - decimal_point = "."
  // - fractional_part = "00"
  // - back_sign = "MI"
  //
  // Note that in addition to conform to the format, a valid format string must
  // also pass other validation rules, described in the doc
  // (broken link), section "Validation"
  // ((broken link)=h.6o0opxcj0jdw). For example, the format
  // string that is given above, "S9,9.00EEEEMI", is in fact not valid since
  // front_sign and back_sign cannot both appear.
  enum class State {
    // The initial state.
    kStart,

    // The state when we're processing the integer part.
    kIntegerPart,

    // The state when we're processing the fractional part.
    kFractionalPart,

    // The state after we have process the exponent element.
    kAfterExponent,

    // The state when we're processing a hexadecimal format string.
    kHexadecimal,

    // The state when the back_sign part has been processed.
    kAfterBackSign,

    // The state when we're processing a RN format string.
    kRomanNumeral,

    // The state when we're processing a text minimal format string.
    kTextMinimal,
  };

  // The current state of the parser.
  State state_ = State::kStart;

  // Indicates whether format element 'X' appears in the format string.
  bool has_x_ = false;

  // Indicates whether format element '9' appears in the format string.
  bool has_9_ = false;

  // Indicates whether a group separator format element appears in the format
  // string.
  bool has_group_separator_ = false;

  // The count of digit elements. It is used for purposes such as verify that
  // the number of hexadecimal digit must be <= 16.
  size_t digit_count_ = 0;

  ParsedFormatElementInfo parsed_format_element_info_;

  // Processes the current format element <element>.
  absl::Status ProcessFormatElement(FormatElement element);

  // Validates after all format element has been processed.
  absl::Status FinalValidate();

  absl::Status OnStartState(FormatElement element);
  absl::Status OnIntegerPartState(FormatElement element);
  absl::Status OnHexadecimalState(FormatElement element);
  absl::Status OnFractionalPartState(FormatElement element);
  absl::Status OnAfterExponentState(FormatElement element);
  absl::Status OnAfterBackSignState(FormatElement element);
};

zetasql_base::StatusBuilder FormatStringErrorBuilder() {
  return zetasql_base::OutOfRangeErrorBuilder() << "Error in format string: ";
}

absl::Status FormatParser::ProcessFormatElement(
    FormatElement element) {
  // Process flag format elements, which can appear anywhere.
  if (element == FormatElement::kCompactMode) {
    if (parsed_format_element_info_.has_fm) {
      return FormatStringErrorBuilder() << "'FM' cannot be repeated";
    } else {
      parsed_format_element_info_.has_fm = true;
      return absl::OkStatus();
    }
  } else if (element == FormatElement::kCurrencyDollar ||
             element == FormatElement::kCurrencyCLower ||
             element == FormatElement::kCurrencyCUpper ||
             element == FormatElement::kCurrencyL) {
    if (parsed_format_element_info_.currency.has_value()) {
      return FormatStringErrorBuilder()
             << "There can be at most one of '$', 'C' or 'L'";
    } else {
      parsed_format_element_info_.currency = element;
      return absl::OkStatus();
    }
  } else if (element == FormatElement::kElementB) {
    if (parsed_format_element_info_.has_b) {
      return FormatStringErrorBuilder() << "There can be at most one 'B'";
    } else {
      parsed_format_element_info_.has_b = true;
      return absl::OkStatus();
    }
  }

  // Update digit_count_.
  if (element == FormatElement::kDigit9 || element == FormatElement::kDigit0 ||
      element == FormatElement::kDigitXLower ||
      element == FormatElement::kDigitXUpper) {
    digit_count_++;
  }

  // Process format elements according to the current state.
  switch (state_) {
    case State::kStart:
      return OnStartState(element);
    case State::kIntegerPart:
      return OnIntegerPartState(element);
    case State::kFractionalPart:
      return OnFractionalPartState(element);
    case State::kAfterExponent:
      return OnAfterExponentState(element);
    case State::kAfterBackSign:
      return OnAfterBackSignState(element);
    case State::kHexadecimal:
      return OnHexadecimalState(element);
    case State::kTextMinimal:
      return FormatStringErrorBuilder()
             << "'TM', 'TM9' or 'TME' cannot be combined with other format "
                "elements";
    case State::kRomanNumeral:
      return FormatStringErrorBuilder()
             << absl::Substitute("'RN' cannot appear together with '$0'",
                                 FormatElementToString(element));
  }

  return absl::OkStatus();
}

absl::Status FormatParser::OnStartState(
    FormatElement element) {
  switch (element) {
    case FormatElement::kSignS:
      parsed_format_element_info_.sign = element;
      state_ = State::kIntegerPart;
      parsed_format_element_info_.sign_at_front = true;
      break;
    case FormatElement::kSignMi:
    case FormatElement::kSignPr:
      return FormatStringErrorBuilder() << absl::Substitute(
                 "'$0' can only appear after all digits and 'EEEE'",
                 FormatElementToString(element));
    case FormatElement::kDigit9:
      has_9_ = true;
      state_ = State::kIntegerPart;
      break;
    case FormatElement::kDigit0:
      state_ = State::kIntegerPart;
      break;
    case FormatElement::kDigitXLower:
    case FormatElement::kDigitXUpper:
      has_x_ = true;
      state_ = State::kHexadecimal;
      break;
    case FormatElement::kRomanNumeralLower:
    case FormatElement::kRomanNumeralUpper:
      parsed_format_element_info_.roman_numeral = element;
      state_ = State::kRomanNumeral;
      break;
    case FormatElement::kDecimalPointDot:
    case FormatElement::kDecimalPointD:
    case FormatElement::kElementV:
      parsed_format_element_info_.decimal_point = element;
      parsed_format_element_info_.decimal_point_index =
          parsed_format_element_info_.elements.size() - 1;
      state_ = State::kFractionalPart;
      break;
    case FormatElement::kTmLower:
    case FormatElement::kTmUpper:
    case FormatElement::kTmeLower:
    case FormatElement::kTmeUpper:
    case FormatElement::kTm9Lower:
    case FormatElement::kTm9Upper:
      parsed_format_element_info_.tm = element;
      state_ = State::kTextMinimal;
      break;
    default:
      return FormatStringErrorBuilder() << absl::Substitute(
                 "Unexpected '$0'", FormatElementToString(element));
  }

  return absl::OkStatus();
}

absl::Status FormatParser::OnIntegerPartState(
    FormatElement element) {
  switch (element) {
    case FormatElement::kSignS:
    case FormatElement::kSignMi:
    case FormatElement::kSignPr:
      if (parsed_format_element_info_.sign.has_value()) {
        return FormatStringErrorBuilder()
               << "There can be at most one of 'S', 'MI', or 'PR'";
      } else {
        parsed_format_element_info_.sign = element;
        state_ = State::kAfterBackSign;
      }
      break;
    case FormatElement::kExponentEeeeLower:
    case FormatElement::kExponentEeeeUpper:
      if (has_group_separator_) {
        return FormatStringErrorBuilder()
               << "',' or 'G' cannot appear together with 'EEEE'";
      } else {
        state_ = State::kAfterExponent;
        parsed_format_element_info_.has_exponent = true;
        parsed_format_element_info_.decimal_point_index =
            parsed_format_element_info_.elements.size() - 1;
      }
      break;
    case FormatElement::kDigitXLower:
    case FormatElement::kDigitXUpper:
      if (has_9_) {
        return FormatStringErrorBuilder()
               << "'X' cannot appear together with '9'";
      }
      if (has_group_separator_) {
        return FormatStringErrorBuilder()
               << "'X' cannot appear together with ',' or 'G'";
      }

      // We reach this point when the format string starts with something like
      // "0X".
      has_x_ = true;
      state_ = State::kHexadecimal;
      break;
    case FormatElement::kDigit9:
      has_9_ = true;
      break;
    case FormatElement::kDigit0:
      break;
    case FormatElement::kGroupSeparatorComma:
    case FormatElement::kGroupSeparatorG:
      has_group_separator_ = true;
      break;
    case FormatElement::kDecimalPointDot:
    case FormatElement::kDecimalPointD:
    case FormatElement::kElementV:
      // decimal_point_ must be nullopt at this point: if there is a decimal
      // point that appears before the state is changed to kIntegerPart, the
      // state would have been transitioned to kFractionalPart, not
      // kIntegerPart.
      ZETASQL_RET_CHECK(!parsed_format_element_info_.decimal_point.has_value());
      parsed_format_element_info_.decimal_point = element;
      parsed_format_element_info_.decimal_point_index =
          parsed_format_element_info_.elements.size() - 1;
      state_ = State::kFractionalPart;
      break;
    case FormatElement::kTmLower:
    case FormatElement::kTmUpper:
    case FormatElement::kTmeLower:
    case FormatElement::kTmeUpper:
    case FormatElement::kTm9Lower:
    case FormatElement::kTm9Upper:
      return FormatStringErrorBuilder()
             << "'TM', 'TM9' or 'TME' cannot be combined with other format "
                "elements";
    default:
      return FormatStringErrorBuilder() << absl::Substitute(
                 "Unexpected '$0'", FormatElementToString(element));
  }

  return absl::OkStatus();
}

absl::Status FormatParser::OnFractionalPartState(FormatElement element) {
  switch (element) {
    case FormatElement::kDigit0:
    case FormatElement::kDigit9:
      parsed_format_element_info_.scale++;
      break;
    case FormatElement::kDigitXLower:
    case FormatElement::kDigitXUpper:
      return FormatStringErrorBuilder() << absl::Substitute(
                 "'X' cannot appear together with '$0'",
                 FormatElementToString(
                     parsed_format_element_info_.decimal_point.value()));
    case FormatElement::kExponentEeeeLower:
    case FormatElement::kExponentEeeeUpper:
      if (has_group_separator_) {
        return FormatStringErrorBuilder()
               << "',' or 'G' cannot appear together with 'EEEE'";
      } else {
        state_ = State::kAfterExponent;
        parsed_format_element_info_.has_exponent = true;
      }
      break;
    case FormatElement::kSignS:
    case FormatElement::kSignMi:
    case FormatElement::kSignPr:
      if (parsed_format_element_info_.sign.has_value()) {
        return FormatStringErrorBuilder()
               << "There can be at most one of 'S', 'MI', or 'PR'";
      } else {
        parsed_format_element_info_.sign = element;
        state_ = State::kAfterBackSign;
      }
      break;
    case FormatElement::kDecimalPointDot:
    case FormatElement::kDecimalPointD:
    case FormatElement::kElementV:
      return FormatStringErrorBuilder()
             << "There can be at most one of '.', 'D', or 'V'";
    case FormatElement::kGroupSeparatorComma:
    case FormatElement::kGroupSeparatorG:
      return FormatStringErrorBuilder()
             << "',' or 'G' cannot appear after '.', 'D' or 'V'";
    default:
      return FormatStringErrorBuilder() << absl::Substitute(
                 "Unexpected '$0'", FormatElementToString(element));
  }

  return absl::OkStatus();
}

absl::Status FormatParser::OnAfterExponentState(FormatElement element) {
  switch (element) {
    case FormatElement::kSignS:
    case FormatElement::kSignMi:
    case FormatElement::kSignPr:
      if (parsed_format_element_info_.sign.has_value()) {
        return FormatStringErrorBuilder()
               << "There can be at most one of 'S', 'MI', or 'PR'";
      } else {
        parsed_format_element_info_.sign = element;
        state_ = State::kAfterBackSign;
      }
      break;
    case FormatElement::kGroupSeparatorComma:
    case FormatElement::kGroupSeparatorG:
      return FormatStringErrorBuilder()
             << "',' or 'G' cannot appear together with 'EEEE'";
    default:
      return FormatStringErrorBuilder()
             << absl::Substitute("'$0' cannot appear after 'EEEE'",
                                 FormatElementToString(element));
  }
  return absl::OkStatus();
}

absl::Status FormatParser::OnAfterBackSignState(
    FormatElement element) {
  switch (element) {
    case FormatElement::kDigit0:
    case FormatElement::kDigit9:
    case FormatElement::kDigitXLower:
    case FormatElement::kDigitXUpper:
    case FormatElement::kExponentEeeeLower:
    case FormatElement::kExponentEeeeUpper:
      if (parsed_format_element_info_.sign.value() == FormatElement::kSignS) {
        return FormatStringErrorBuilder()
               << "'S' can only appear before or after all digits and 'EEEE'";
      } else {
        return FormatStringErrorBuilder() << absl::Substitute(
                   "'$0' can only appear after all digits and 'EEEE'",
                   FormatElementToString(
                       parsed_format_element_info_.sign.value()));
      }
    default:
      return FormatStringErrorBuilder()
             << absl::Substitute("Unexpected format element '$0'",
                                 FormatElementToString(element));
  }
  return absl::OkStatus();
}

absl::Status FormatParser::OnHexadecimalState(
    FormatElement element) {
  switch (element) {
    case FormatElement::kDigit0:
    case FormatElement::kDigitXLower:
    case FormatElement::kDigitXUpper:
      break;
    case FormatElement::kSignS:
    case FormatElement::kSignMi:
    case FormatElement::kSignPr:
      if (parsed_format_element_info_.sign.has_value()) {
        return FormatStringErrorBuilder()
               << "There can be at most one of 'S', 'MI', or 'PR'";
      } else {
        parsed_format_element_info_.sign = element;
        state_ = State::kAfterBackSign;
      }
      break;
    default:
      return FormatStringErrorBuilder()
             << absl::Substitute("'X' cannot appear together with '$0'",
                                 FormatElementToString(element));
  }
  return absl::OkStatus();
}

absl::Status FormatParser::FinalValidate() {
  if (parsed_format_element_info_.currency.has_value()) {
    if (parsed_format_element_info_.tm.has_value()) {
      return FormatStringErrorBuilder()
             << "'TM', 'TM9' or 'TME' cannot be combined with other format "
                "elements";
    } else if (has_x_) {
      return FormatStringErrorBuilder() << absl::Substitute(
                 "'X' cannot appear together with '$0'",
                 FormatElementToString(
                     parsed_format_element_info_.currency.value()));
    } else if (parsed_format_element_info_.roman_numeral.has_value()) {
      return FormatStringErrorBuilder() << absl::Substitute(
                 "'RN' cannot appear together with '$0'",
                 FormatElementToString(
                     parsed_format_element_info_.currency.value()));
    }
  }

  if (parsed_format_element_info_.has_b) {
    if (parsed_format_element_info_.tm.has_value()) {
      return FormatStringErrorBuilder()
             << "'TM', 'TM9' or 'TME' cannot be combined "
                "with other format elements";
    } else if (has_x_) {
      return FormatStringErrorBuilder()
             << "'X' cannot appear together with 'B'";
    } else if (parsed_format_element_info_.roman_numeral.has_value()) {
      return FormatStringErrorBuilder()
             << "'RN' cannot appear together with 'B'";
    }
  }

  if (parsed_format_element_info_.has_fm) {
    if (parsed_format_element_info_.tm.has_value()) {
      return FormatStringErrorBuilder()
             << "'TM', 'TM9' or 'TME' cannot be combined with other format "
                "elements";
    }
  }

  if (parsed_format_element_info_.tm.has_value() ||
      parsed_format_element_info_.roman_numeral.has_value()) {
    return absl::OkStatus();
  }

  if (digit_count_ == 0) {
    return FormatStringErrorBuilder()
           << "Format string must contain at least one of 'X', '0' or '9'";
  }

  if (has_x_ && digit_count_ > 16) {
    return FormatStringErrorBuilder() << "Max number of 'X' is 16";
  }

  return absl::OkStatus();
}

}  // namespace

std::string FormatElementToString(FormatElement element) {
  // The returned strings are always in uppercase, so that the error messages
  // will be the same regardless of the letter cases in the format string. This
  // makes testing a little bit easier since we do not need to provide two error
  // messages for the same test.
  switch (element) {
    case FormatElement::kCurrencyDollar:
      return "$";
    case FormatElement::kDigit0:
      return "0";
    case FormatElement::kDigit9:
      return "9";
    case FormatElement::kDigitXLower:
    case FormatElement::kDigitXUpper:
      return "X";
    case FormatElement::kDecimalPointDot:
      return ".";
    case FormatElement::kGroupSeparatorComma:
      return ",";
    case FormatElement::kSignS:
      return "S";
    case FormatElement::kSignMi:
      return "MI";
    case FormatElement::kSignPr:
      return "PR";
    case FormatElement::kRomanNumeralLower:
    case FormatElement::kRomanNumeralUpper:
      return "RN";
    case FormatElement::kExponentEeeeLower:
    case FormatElement::kExponentEeeeUpper:
      return "EEEE";
    case FormatElement::kElementB:
      return "B";
    case FormatElement::kElementV:
      return "V";
    case FormatElement::kCompactMode:
      return "FM";
    case FormatElement::kTm9Lower:
    case FormatElement::kTm9Upper:
      return "TM9";
    case FormatElement::kTmeLower:
    case FormatElement::kTmeUpper:
      return "TME";
    case FormatElement::kTmLower:
    case FormatElement::kTmUpper:
      return "TM";
    case FormatElement::kCurrencyCLower:
    case FormatElement::kCurrencyCUpper:
      return "C";
    case FormatElement::kCurrencyL:
      return "L";
    case FormatElement::kDecimalPointD:
      return "D";
    case FormatElement::kGroupSeparatorG:
      return "G";
  }
}

// Gets the format element at the start of the input string <str>.
// Upon return, <length> contains the length of the format element in <str>, and
// <upper> indicates whether the format element is in uppercase or lowercase.
// E.g. if str is "9.9", returns FormatElement::kDigit9, and length is 1.
//
// If there is no valid format element, returns nullopt.
absl::optional<FormatElement> GetFormatElement(absl::string_view str,
                                               int& length) {
  if (str.empty()) {
    length = 0;
    return absl::nullopt;
  }

  length = 1;
  switch (str[0]) {
    case '$':
      return FormatElement::kCurrencyDollar;
    case '0':
      return FormatElement::kDigit0;
    case '9':
      return FormatElement::kDigit9;
    case 'X':
      return FormatElement::kDigitXUpper;
    case 'x':
      return FormatElement::kDigitXLower;
    case '.':
      return FormatElement::kDecimalPointDot;
    case 'D':
    case 'd':
      return FormatElement::kDecimalPointD;
    case ',':
      return FormatElement::kGroupSeparatorComma;
    case 'G':
    case 'g':
      return FormatElement::kGroupSeparatorG;
    case 'S':
    case 's':
      return FormatElement::kSignS;
    case 'M':
    case 'm':
      if (absl::StartsWithIgnoreCase(str, "MI")) {
        length = 2;
        return FormatElement::kSignMi;
      } else {
        return absl::nullopt;
      }
    case 'P':
    case 'p':
      if (absl::StartsWithIgnoreCase(str, "PR")) {
        length = 2;
        return FormatElement::kSignPr;
      } else {
        return absl::nullopt;
      }
    case 'R':
      if (absl::StartsWithIgnoreCase(str, "RN")) {
        length = 2;
        return FormatElement::kRomanNumeralUpper;
      } else {
        return absl::nullopt;
      }
    case 'r':
      if (absl::StartsWithIgnoreCase(str, "RN")) {
        length = 2;
        return FormatElement::kRomanNumeralLower;
      } else {
        return absl::nullopt;
      }
    case 'E':
      if (absl::StartsWithIgnoreCase(str, "EEEE")) {
        length = 4;
        return FormatElement::kExponentEeeeUpper;
      } else {
        return absl::nullopt;
      }
    case 'e':
      if (absl::StartsWithIgnoreCase(str, "EEEE")) {
        length = 4;
        return FormatElement::kExponentEeeeLower;
      } else {
        return absl::nullopt;
      }
    case 'B':
    case 'b':
      return FormatElement::kElementB;
    case 'V':
    case 'v':
      return FormatElement::kElementV;
    case 'F':
    case 'f':
      if (absl::StartsWithIgnoreCase(str, "FM")) {
        length = 2;
        return FormatElement::kCompactMode;
      } else {
        return absl::nullopt;
      }
    case 'T':
      if (absl::StartsWithIgnoreCase(str, "TM9")) {
        length = 3;
        return FormatElement::kTm9Upper;
      } else if (absl::StartsWithIgnoreCase(str, "TME")) {
        length = 3;
        return FormatElement::kTmeUpper;
      } else if (absl::StartsWithIgnoreCase(str, "TM")) {
        length = 2;
        return FormatElement::kTmUpper;
      } else {
        return absl::nullopt;
      }
    case 't':
      if (absl::StartsWithIgnoreCase(str, "TM9")) {
        length = 3;
        return FormatElement::kTm9Lower;
      } else if (absl::StartsWithIgnoreCase(str, "TME")) {
        length = 3;
        return FormatElement::kTmeLower;
      } else if (absl::StartsWithIgnoreCase(str, "TM")) {
        length = 2;
        return FormatElement::kTmLower;
      } else {
        return absl::nullopt;
      }
    case 'C':
      return FormatElement::kCurrencyCUpper;
    case 'c':
      return FormatElement::kCurrencyCLower;
    case 'L':
    case 'l':
      return FormatElement::kCurrencyL;
    default:
      return absl::nullopt;
  }
}

zetasql_base::StatusOr<ParsedFormatElementInfo> FormatParser::Parse(
    absl::string_view format) {
  if (format.size() > absl::GetFlag(FLAGS_zetasql_format_max_output_width)) {
    return FormatStringErrorBuilder()
           << "Format string too long; limit "
           << absl::GetFlag(FLAGS_zetasql_format_max_output_width);
  }

  parsed_format_element_info_.decimal_point_index = std::string::npos;
  int index = 0;
  while (index < format.size()) {
    int length;
    absl::optional<FormatElement> element =
        GetFormatElement(format.substr(index), length);
    if (element.has_value()) {
      // Add element to parsed_format_element_info_.elements if the element is
      // - a digit
      // - a decimal point or 'V'
      // - a group separator
      // - the exponent, i.e. 'EEEE'
      switch (element.value()) {
        case FormatElement::kDigit0:
          parsed_format_element_info_.elements.push_back(element.value());
          if (!parsed_format_element_info_.index_of_first_zero.has_value()) {
            parsed_format_element_info_.index_of_first_zero =
                parsed_format_element_info_.elements.size() - 1;
          }
          break;
        case FormatElement::kDigitXLower:
        case FormatElement::kDigitXUpper:
        case FormatElement::kDigit9:
        case FormatElement::kDecimalPointDot:
        case FormatElement::kDecimalPointD:
        case FormatElement::kElementV:
        case FormatElement::kGroupSeparatorComma:
        case FormatElement::kGroupSeparatorG:
        case FormatElement::kExponentEeeeLower:
        case FormatElement::kExponentEeeeUpper:
          parsed_format_element_info_.elements.push_back(element.value());
          break;
        default:
          // no-op
          break;
      }
      index += length;
      ZETASQL_RETURN_IF_ERROR(ProcessFormatElement(element.value()));
    } else {
      return FormatStringErrorBuilder() << absl::Substitute(
                 "Invalid format element '$0'", format.substr(index, 1));
    }
  }

  ZETASQL_RETURN_IF_ERROR(FinalValidate());

  if (parsed_format_element_info_.tm.has_value()) {
    parsed_format_element_info_.output_type = OutputType::kTextMinimal;
  } else if (parsed_format_element_info_.roman_numeral.has_value()) {
    parsed_format_element_info_.output_type = OutputType::kRomanNumeral;
  } else if (has_x_) {
    parsed_format_element_info_.output_type = OutputType::kHexadecimal;
  } else {
    parsed_format_element_info_.output_type = OutputType::kDecimal;
    parsed_format_element_info_.num_integer_digit =
        digit_count_ - parsed_format_element_info_.scale;

    // Sets decimal_point_index if it's not set. For example, this happens when
    // the format is "9999".
    if (parsed_format_element_info_.decimal_point_index == std::string::npos) {
      parsed_format_element_info_.decimal_point_index =
          parsed_format_element_info_.elements.size();
    }

    if (parsed_format_element_info_.has_exponent) {
      // The spec requires that at most one integer digit is kept if exponent is
      // specified. Here we delete extra integer digits if there are more than
      // one. So, if the elements array contains "999.99EEEE", it will be turned
      // into "9.99EEEE".
      // Because group separators cannot appear together with exponent,
      // only digits can appear before the decimal point. Thus,
      // decimal_point_index_ is the number of integer digits.
      if (parsed_format_element_info_.decimal_point_index >= 2) {
        parsed_format_element_info_.elements.erase(
            parsed_format_element_info_.elements.begin(),
            parsed_format_element_info_.elements.begin() +
                parsed_format_element_info_.decimal_point_index - 1);
        parsed_format_element_info_.decimal_point_index = 1;
      }
    }
  }

  return parsed_format_element_info_;
}

// Generates and returns the fractional part of the output.
zetasql_base::StatusOr<std::string> GenerateFractionalPart(
    const ParsedFormatElementInfo& parsed_format_element_info,
    const ParsedNumberString& n) {
  std::string result;
  bool overflow =
      n.integer_part.size() > parsed_format_element_info.num_integer_digit;
  size_t fractional_part_index = 0;
  for (size_t format_index = parsed_format_element_info.decimal_point_index;
       format_index < parsed_format_element_info.elements.size();
       ++format_index) {
    switch (parsed_format_element_info.elements[format_index]) {
      case FormatElement::kDecimalPointDot:
      case FormatElement::kDecimalPointD:
        result.push_back('.');
        break;
      case FormatElement::kElementV:
        // 'V' generates zero output
        break;
      case FormatElement::kDigit9:
      case FormatElement::kDigit0:
        if (overflow) {
          result.append("#");
        } else {
          if (fractional_part_index < n.fractional_part.size()) {
            result.append(1, n.fractional_part[fractional_part_index]);
          } else {
            // Note that we don't append '0' here. We reach this point only when
            // FM is specified. For example, when input is 1.2, format string is
            // "9.999FM", then n.fractional_part would be "2", not "200". Since
            // FM is specified, we do not generate any trailing 0s. Thus, no-op
            // here.
          }
        }
        fractional_part_index++;
        break;
      case FormatElement::kExponentEeeeLower:
        if (overflow) {
          result.append("####");
        } else {
          result.append("e");
          result.append(n.exponent);
        }
        break;
      case FormatElement::kExponentEeeeUpper:
        if (overflow) {
          result.append("####");
        } else {
          result.append("E");
          result.append(n.exponent);
        }
        break;
      default:
        ZETASQL_RET_CHECK_FAIL()
            << "Should never happen. Unexpected format element at index "
            << format_index << ": "
            << FormatElementToString(
                   parsed_format_element_info.elements[format_index]);
        break;
    }
  }

  return result;
}

struct IntegerPart {
  std::string text;
  int left_padding_size;
};

// Generates and returns the integer part. Upon returns, <text> of the return
// value is the string containing the integer part of the output, and
// <left_padding_size> of the return value contains the number of spaces that
// should be left padded the output.  For example, for input 12.3, format string
// "9999.99", calling this method returns { text = "12", left_padding_size = 2}.
zetasql_base::StatusOr<IntegerPart> FormatIntegerPartOfDecimal(
    const ParsedFormatElementInfo& parsed_format_element_info,
    const ParsedNumberString& n) {
  std::string result;
  result.reserve(parsed_format_element_info.decimal_point_index);

  bool overflow =
      n.integer_part.size() > parsed_format_element_info.num_integer_digit;
  absl::string_view integer_part = "0";
  if (!n.integer_part.empty()) {
    integer_part = n.integer_part;
  }

  // For performance reason, the output is generated backward.
  int integer_part_index = static_cast<int>(integer_part.size()) - 1;
  int format_index =
      static_cast<int>(parsed_format_element_info.decimal_point_index) - 1;
  for (; format_index >= 0; format_index--) {
    if (integer_part_index < 0) {
      // All digits from the integer part have been added to the output. Now we
      // check if we need to generate leading 0s.
      if (parsed_format_element_info.index_of_first_zero.has_value() &&
          format_index >=
              parsed_format_element_info.index_of_first_zero.value()) {
        // Yes, there is a '0' format element before the current position, thus,
        // we need to continue to generate leading 0s in the output.
      } else {
        // There is no '0' format element before the current position. So we
        // stop.
        break;
      }
    }

    switch (parsed_format_element_info.elements[format_index]) {
      case FormatElement::kDigit0:
      case FormatElement::kDigit9:
        if (overflow) {
          result.push_back('#');
        } else {
          char digit =
              integer_part_index >= 0 ? integer_part[integer_part_index] : '0';
          result.push_back(digit);
        }
        integer_part_index--;
        break;
      case FormatElement::kGroupSeparatorComma:
      case FormatElement::kGroupSeparatorG:
        result.push_back(',');
        break;
      default:
        ZETASQL_RET_CHECK_FAIL()
            << "Should never happen. Unexpected format element at index "
            << format_index << " : "
            << FormatElementToString(
                   parsed_format_element_info.elements[format_index]);
        break;
    }
  }

  // Because the output is generated backward, we need to reverse it.
  std::reverse(result.begin(), result.end());

  return IntegerPart{.text = result,
                     .left_padding_size = format_index + 1};
}

zetasql_base::StatusOr<std::string> GenerateCurrencyOutput(
    const ParsedFormatElementInfo& parsed_format_element_info) {
  std::string result;

  if (parsed_format_element_info.currency.has_value()) {
    switch (parsed_format_element_info.currency.value()) {
      case FormatElement::kCurrencyDollar:
      case FormatElement::kCurrencyL:
        result = "$";
        break;
      case FormatElement::kCurrencyCLower:
        result = "usd";
        break;
      case FormatElement::kCurrencyCUpper:
        result = "USD";
        break;
      default:
        ZETASQL_RET_CHECK_FAIL() << "Should never happen. Unexpected format element: "
                         << FormatElementToString(
                                parsed_format_element_info.currency.value());
    }
  }

  return result;
}

// Represents the output generated for the sign. It contains parts: <prefix>
// is the part that will be prepended to the number, and <suffix> is the part
// that will be appended to the number.
struct SignOutput {
  std::string prefix;
  std::string suffix;
};

// Generate the output for sign.
zetasql_base::StatusOr<SignOutput> GenerateSignOutput(
    bool negative, const ParsedFormatElementInfo& parsed_format_element_info) {
  std::string prefix, suffix;

  // Generate sign.
  if (parsed_format_element_info.sign.has_value()) {
    switch (parsed_format_element_info.sign.value()) {
      case FormatElement::kSignS:
        if (parsed_format_element_info.sign_at_front) {
          prefix = negative ? "-" : "+";
        } else {
          suffix = negative ? "-" : "+";
        }
        break;
      case FormatElement::kSignMi:
        suffix = negative ? "-" : " ";
        break;
      case FormatElement::kSignPr:
        prefix = negative ? "<" : " ";
        suffix = negative ? ">" : " ";
        break;
      default:
        ZETASQL_RET_CHECK_FAIL() << "Should never happen. The sign element is:"
                         << FormatElementToString(
                                parsed_format_element_info.sign.value());
        break;
    }
  } else {
    // Sign is not specified in the format string.
    prefix = negative ? "-" : " ";
  }

  return SignOutput{.prefix = prefix, .suffix = suffix};
}

zetasql_base::StatusOr<std::string> FormatAsDecimalInternal(
    const ParsedFormatElementInfo& parsed_format_element_info,
    const ParsedNumberString& n) {
  if (n.is_infinity || n.is_nan) {
    // TODO: support INF and NAN
    return zetasql_base::UnimplementedErrorBuilder() << "INF/NAN is not supported yet";
  }

  if (parsed_format_element_info.has_b ||
      parsed_format_element_info.has_fm) {
    // TODO: implement support for 'B' and 'FM'.
    return zetasql_base::UnimplementedErrorBuilder()
           << "'B', 'FM', sign and currency are not implemented yet";
  }

  // Generate fractional part.
  ZETASQL_ASSIGN_OR_RETURN(std::string fractional_part,
                   GenerateFractionalPart(parsed_format_element_info, n));

  IntegerPart integer_part = {
      .text = "",
      .left_padding_size =
          static_cast<int>(parsed_format_element_info.decimal_point_index),
  };

  if (parsed_format_element_info.num_integer_digit > 0) {
    // Check whether the integer part should be generated.  For example, for
    // value 0.12 and format "9.99", we do not generate the integer part, and
    // the output would be " .12".
    bool generate_integer_part = false;
    if (!n.integer_part.empty()) {
      generate_integer_part = true;
    } else if (parsed_format_element_info.has_exponent) {
      generate_integer_part = true;
    } else if (parsed_format_element_info.index_of_first_zero.has_value() &&
               parsed_format_element_info.index_of_first_zero.value() <
                   parsed_format_element_info.decimal_point_index) {
      // There is "0" in the integer part of the format string, so at least one
      // digit needs to be generated in the integer part of the output.
      generate_integer_part = true;
    } else if (n.fractional_part.empty()) {
      // Both integer_part and fractional_part are empty. In this case, we need
      // to generate the integer part.
      generate_integer_part = true;
    }

    if (generate_integer_part) {
      ZETASQL_ASSIGN_OR_RETURN(integer_part, FormatIntegerPartOfDecimal(
                                         parsed_format_element_info, n));
    }
  }

  ZETASQL_ASSIGN_OR_RETURN(
      std::string currency_output,
      GenerateCurrencyOutput(parsed_format_element_info));
  ZETASQL_ASSIGN_OR_RETURN(SignOutput sign_output,
                   GenerateSignOutput(n.negative, parsed_format_element_info));
  std::string left_padding = std::string(integer_part.left_padding_size, ' ');

  return absl::StrCat(left_padding, sign_output.prefix, currency_output,
                      integer_part.text, fractional_part, sign_output.suffix);
}

zetasql_base::StatusOr<std::string> FormatAsDecimal(
    const ParsedFormatElementInfo& parsed_format_element_info,
    const Value& v,
    ProductMode product_mode) {
  if (v.type()->IsInteger()) {
    // If the input value is an integer, convert it to NUMERIC first.
    Value new_value;
    switch (v.type()->kind()) {
      case TYPE_INT32:
        new_value = Value::Numeric(NumericValue(v.int32_value()));
        break;
      case TYPE_UINT32:
        new_value = Value::Numeric(NumericValue(v.uint32_value()));
        break;
      case TYPE_INT64:
        new_value = Value::Numeric(NumericValue(v.int64_value()));
        break;
      case TYPE_UINT64:
        new_value = Value::Numeric(NumericValue(v.uint64_value()));
        break;
      default:
        ZETASQL_RET_CHECK_FAIL() << "Should never reach here. Input value: "
                         << v.DebugString();
        break;
    }

    return FormatAsDecimal(parsed_format_element_info, new_value, product_mode);
  }

  // Formats the input value by calling FORMAT().
  ZETASQL_RET_CHECK(v.type()->IsFloatingPoint() || v.type()->IsNumericType() ||
            v.type()->IsBigNumericType());

  std::string format_string =
      absl::Substitute("%#.$0$1", parsed_format_element_info.scale,
                       parsed_format_element_info.has_exponent ? 'e' : 'f');

  bool is_null = false;
  std::string numeric_string;
  ZETASQL_RETURN_IF_ERROR(zetasql::functions::StringFormatUtf8(
      format_string, {v}, product_mode, &numeric_string, &is_null));
  ZETASQL_RET_CHECK(!is_null);

  ZETASQL_ASSIGN_OR_RETURN(ParsedNumberString n,
                   ParseFormattedRealNumber(numeric_string));

  // Generates the output.
  return FormatAsDecimalInternal(parsed_format_element_info, n);
}

zetasql_base::StatusOr<ParsedNumberString> ParseFormattedRealNumber(
    absl::string_view number_string) {
  ParsedNumberString output;
  if (number_string == "inf") {
    output.is_infinity = true;
  } else if (number_string == "-inf") {
    output.negative = true;
    output.is_infinity = true;
  } else if (number_string == "nan") {
    output.is_nan = true;
  } else {
    // A sanity check of the format of number_string. It is generated using a
    // format string that is either "%#.4f" or "%#.4e", thus it must match the
    // regex used here.
    ZETASQL_RET_CHECK(
        RE2::FullMatch(number_string, "-?[0-9]+\\.[0-9]*(e(\\+|-)[0-9]+)?"))
        << "Input: " << number_string;

    // decimal point is guaranteed to exist in number_string, since we used flag
    // "#" in the format string passed to FORMAT().
    size_t decimal_point_pos = number_string.find_first_of('.');
    ZETASQL_RET_CHECK(decimal_point_pos != absl::string_view::npos);

    size_t e_pos = number_string.find_first_of('e');
    if (e_pos != absl::string_view::npos) {
      output.exponent = number_string.substr(e_pos + 1);
      number_string = number_string.substr(0, e_pos);
    }

    output.fractional_part = number_string.substr(decimal_point_pos + 1);

    if (number_string[0] == '-') {
      output.negative = true;
      output.integer_part = number_string.substr(1, decimal_point_pos - 1);
    } else {
      output.negative = false;
      output.integer_part = number_string.substr(0, decimal_point_pos);
    }

    if (output.integer_part == "0") {
      output.integer_part = "";
    }
  }

  return output;
}

zetasql_base::StatusOr<ParsedFormatElementInfo> ParseForTest(absl::string_view format) {
  FormatParser parser;
  ZETASQL_ASSIGN_OR_RETURN(ParsedFormatElementInfo parsed_format_element_info,
                   parser.Parse(format));
  return parsed_format_element_info;
}

}  // namespace internal

absl::Status ValidateNumericalToStringFormat(absl::string_view format) {
  internal::FormatParser parser;
  ZETASQL_ASSIGN_OR_RETURN(internal::ParsedFormatElementInfo parsed_format_element_info,
                   parser.Parse(format));
  return absl::OkStatus();
}

zetasql_base::StatusOr<std::string> NumericalToStringWithFormat(
    const Value& v, absl::string_view format, ProductMode product_mode) {
  ZETASQL_RET_CHECK(!v.is_null());

  internal::FormatParser parser;
  ZETASQL_ASSIGN_OR_RETURN(internal::ParsedFormatElementInfo parsed_format_element_info,
                   parser.Parse(format));
  switch (parsed_format_element_info.output_type) {
    case internal::OutputType::kDecimal:
      return FormatAsDecimal(parsed_format_element_info, v, product_mode);
    case internal::OutputType::kTextMinimal:
      return zetasql_base::UnimplementedErrorBuilder()
             << "Text minimal output is not supported yet";
    case internal::OutputType::kHexadecimal:
      return zetasql_base::UnimplementedErrorBuilder()
             << "Hexadecimal output is not supported yet";
    case internal::OutputType::kRomanNumeral:
      return zetasql_base::UnimplementedErrorBuilder()
             << "Roman numeral output is not supported yet";
  }
}

}  // namespace functions
}  // namespace zetasql
