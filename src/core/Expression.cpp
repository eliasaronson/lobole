#include "core/Expression.h"

#include "core/Angles.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <vector>

namespace {

// Unit suffixes (C2 units algebra): a factor that converts a unit-suffixed
// literal to the document's base unit — millimetres for length, degrees for
// angle. There is deliberately NO dimensional analysis: a bare number is
// already in base units, the field context (length vs angle) decides which
// base applies, and mixing (`10mm + 45deg`) is arithmetically legal nonsense.
// nullopt = not a unit name.
std::optional<double> UnitFactor(const std::string& name) {
  static const std::unordered_map<std::string, double> kUnits = {
      {"mm", 1.0},
      {"cm", 10.0},
      {"m", 1000.0},
      {"in", 25.4},
      {"ft", 304.8},
      {"deg", 1.0},
      {"rad", angles::kDegPerRad},
  };
  const auto it = kUnits.find(name);
  return it == kUnits.end() ? std::nullopt : std::optional<double>(it->second);
}

// ---------------------------------------------------------------- Tokenizer

enum class TokKind {
  Number,
  Ident,
  Plus,
  Minus,
  Star,
  Slash,
  Caret,
  LParen,
  RParen,
  Comma,
  End
};

struct Token {
  TokKind kind = TokKind::End;
  size_t offset = 0;
  size_t length = 0;
  double number = 0.0; // Number only
  std::string text;    // Ident only
};

// Tokenizes the whole string. Returns false and fills outError on an
// unexpected character or malformed number.
bool Tokenize(const std::string& src, std::vector<Token>& out,
              ExprError* outError) {
  size_t i = 0;
  const size_t n = src.size();
  while (i < n) {
    const char c = src[i];
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      ++i;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
        (c == '.' && i + 1 < n &&
         std::isdigit(static_cast<unsigned char>(src[i + 1])) != 0)) {
      const char* begin = src.c_str() + i;
      char* end = nullptr;
      const double value = std::strtod(begin, &end);
      const size_t len = static_cast<size_t>(end - begin);
      if (len == 0 || !std::isfinite(value)) {
        if (outError != nullptr) {
          *outError = {"invalid number", i, 1};
        }
        return false;
      }
      // Optional unit suffix (`10mm`, `10 mm`, `1.5in`): if an identifier-run
      // immediately follows the number (past optional whitespace) and is a
      // known unit, fold its factor into the literal and consume it. It never
      // becomes an Ident token, so Dependencies()/RenameIdentifier never see
      // it. "Units win after a number": the suffix binds here even if a
      // same-named parameter could exist (parameters can't be named after a
      // unit — see IsValidParameterName). A non-unit run is left alone (it
      // tokenizes as the next Ident, an error after a number, exactly as
      // before units existed).
      size_t after = i + len;
      size_t u = after;
      while (u < n && std::isspace(static_cast<unsigned char>(src[u])) != 0) {
        ++u;
      }
      if (u < n && (std::isalpha(static_cast<unsigned char>(src[u])) != 0 ||
                    src[u] == '_')) {
        size_t e = u + 1;
        while (e < n &&
               (std::isalnum(static_cast<unsigned char>(src[e])) != 0 ||
                src[e] == '_')) {
          ++e;
        }
        if (const auto factor = UnitFactor(src.substr(u, e - u))) {
          out.push_back({TokKind::Number, i, e - i, value * *factor, {}});
          i = e;
          continue;
        }
      }
      out.push_back({TokKind::Number, i, len, value, {}});
      i += len;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
      size_t j = i + 1;
      while (j < n && (std::isalnum(static_cast<unsigned char>(src[j])) != 0 ||
                       src[j] == '_')) {
        ++j;
      }
      out.push_back({TokKind::Ident, i, j - i, 0.0, src.substr(i, j - i)});
      i = j;
      continue;
    }
    TokKind kind = TokKind::End;
    switch (c) {
    case '+':
      kind = TokKind::Plus;
      break;
    case '-':
      kind = TokKind::Minus;
      break;
    case '*':
      kind = TokKind::Star;
      break;
    case '/':
      kind = TokKind::Slash;
      break;
    case '^':
      kind = TokKind::Caret;
      break;
    case '(':
      kind = TokKind::LParen;
      break;
    case ')':
      kind = TokKind::RParen;
      break;
    case ',':
      kind = TokKind::Comma;
      break;
    default:
      if (outError != nullptr) {
        *outError = {std::string("unexpected character '") + c + "'", i, 1};
      }
      return false;
    }
    out.push_back({kind, i, 1, 0.0, {}});
    ++i;
  }
  out.push_back({TokKind::End, n, 0, 0.0, {}});
  return true;
}

// ---------------------------------------------------------------------- AST

struct Node {
  enum class Kind { Number, Ident, Unary, Binary, Call };

  Kind kind = Kind::Number;
  size_t offset = 0; // span in the source text, for evaluation errors
  size_t length = 0;
  double number = 0.0;                     // Number
  std::string name;                        // Ident / Call
  char op = 0;                             // Unary ('-') / Binary
  std::vector<std::unique_ptr<Node>> args; // Unary/Binary operands, Call args
};

// Built-in functions: name -> {min args, max args}. Trig is in degrees to
// match the dimension UI.
struct FuncSig {
  size_t minArgs = 0;
  size_t maxArgs = 0;
};
const std::unordered_map<std::string, FuncSig>& Functions() {
  static const std::unordered_map<std::string, FuncSig> f = {
      {"sin", {1, 1}},  {"cos", {1, 1}},        {"tan", {1, 1}},
      {"sqrt", {1, 1}}, {"abs", {1, 1}},        {"floor", {1, 1}},
      {"ceil", {1, 1}}, {"min", {2, SIZE_MAX}}, {"max", {2, SIZE_MAX}},
  };
  return f;
}

bool IsReservedName(const std::string& name) {
  return name == "pi" || Functions().contains(name);
}

// ------------------------------------------------------------------- Parser
//
// expression := additive
// additive   := multiplicative (('+' | '-') multiplicative)*
// multiplicative := unary (('*' | '/') unary)*
// unary      := '-' unary | power
// power      := primary ('^' unary)?          right-assoc; exponent may be
//                                             signed, so 2^-3 parses
// primary    := number | ident | ident '(' expression (',' expression)* ')'
//             | '(' expression ')'

class Parser {
public:
  Parser(const std::vector<Token>& toks, ExprError* outError)
      : m_toks(toks), m_error(outError) {}

  std::unique_ptr<Node> Parse() {
    auto node = ParseAdditive();
    if (!node) {
      return nullptr;
    }
    if (Peek().kind != TokKind::End) {
      Fail(Peek(), "unexpected trailing input");
      return nullptr;
    }
    return node;
  }

private:
  [[nodiscard]] const Token& Peek() const { return m_toks[m_pos]; }
  const Token& Take() { return m_toks[m_pos++]; }
  bool Accept(TokKind kind) {
    if (Peek().kind != kind) {
      return false;
    }
    ++m_pos;
    return true;
  }
  void Fail(const Token& at, const std::string& message) {
    if (m_error != nullptr) {
      *m_error = {message, at.offset, at.length != 0 ? at.length : 1};
    }
  }

  static std::unique_ptr<Node> Make(Node::Kind kind, size_t offset,
                                    size_t length) {
    auto n = std::make_unique<Node>();
    n->kind = kind;
    n->offset = offset;
    n->length = length;
    return n;
  }

  static char AdditiveOp(TokKind kind) {
    if (kind == TokKind::Plus) {
      return '+';
    }
    return kind == TokKind::Minus ? '-' : '\0';
  }

  static char MultiplicativeOp(TokKind kind) {
    if (kind == TokKind::Star) {
      return '*';
    }
    return kind == TokKind::Slash ? '/' : '\0';
  }

  std::unique_ptr<Node> ParseAdditive() {
    auto lhs = ParseMultiplicative();
    while (lhs) {
      const char op = AdditiveOp(Peek().kind);
      if (op == '\0') {
        break;
      }
      Take();
      auto rhs = ParseMultiplicative();
      if (!rhs) {
        return nullptr;
      }
      lhs = MakeBinary(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
  }

  std::unique_ptr<Node> ParseMultiplicative() {
    auto lhs = ParseUnary();
    while (lhs) {
      const char op = MultiplicativeOp(Peek().kind);
      if (op == '\0') {
        break;
      }
      Take();
      auto rhs = ParseUnary();
      if (!rhs) {
        return nullptr;
      }
      lhs = MakeBinary(op, std::move(lhs), std::move(rhs));
    }
    return lhs;
  }

  std::unique_ptr<Node> ParseUnary() {
    if (Peek().kind == TokKind::Minus) {
      const Token tok = Take();
      auto operand = ParseUnary();
      if (!operand) {
        return nullptr;
      }
      auto node = Make(Node::Kind::Unary, tok.offset,
                       operand->offset + operand->length - tok.offset);
      node->op = '-';
      node->args.push_back(std::move(operand));
      return node;
    }
    return ParsePower();
  }

  std::unique_ptr<Node> ParsePower() {
    auto base = ParsePrimary();
    if (!base) {
      return nullptr;
    }
    if (!Accept(TokKind::Caret)) {
      return base;
    }
    auto exponent = ParseUnary(); // right-assoc + signed exponents
    if (!exponent) {
      return nullptr;
    }
    return MakeBinary('^', std::move(base), std::move(exponent));
  }

  std::unique_ptr<Node> ParsePrimary() {
    const Token tok = Peek();
    switch (tok.kind) {
    case TokKind::Number: {
      Take();
      auto node = Make(Node::Kind::Number, tok.offset, tok.length);
      node->number = tok.number;
      return node;
    }
    case TokKind::Ident: {
      Take();
      if (Peek().kind == TokKind::LParen) {
        return ParseCall(tok);
      }
      auto node = Make(Node::Kind::Ident, tok.offset, tok.length);
      node->name = tok.text;
      return node;
    }
    case TokKind::LParen: {
      Take();
      auto inner = ParseAdditive();
      if (!inner) {
        return nullptr;
      }
      if (!Accept(TokKind::RParen)) {
        Fail(Peek(), "expected ')'");
        return nullptr;
      }
      return inner;
    }
    case TokKind::End:
      Fail(tok, "unexpected end of expression");
      return nullptr;
    default:
      Fail(tok, "unexpected token");
      return nullptr;
    }
  }

  // Called with the identifier token taken and '(' pending.
  std::unique_ptr<Node> ParseCall(const Token& name) {
    Take(); // '('
    auto call = Make(Node::Kind::Call, name.offset, name.length);
    call->name = name.text;
    while (true) {
      auto arg = ParseAdditive();
      if (!arg) {
        return nullptr;
      }
      call->args.push_back(std::move(arg));
      if (!Accept(TokKind::Comma)) {
        break;
      }
    }
    const Token close = Peek();
    if (!Accept(TokKind::RParen)) {
      Fail(close, "expected ')'");
      return nullptr;
    }
    call->length = close.offset + close.length - call->offset;
    const auto it = Functions().find(call->name);
    if (it == Functions().end()) {
      Fail(name, "unknown function '" + call->name + "'");
      return nullptr;
    }
    if (call->args.size() < it->second.minArgs ||
        call->args.size() > it->second.maxArgs) {
      Fail(name, "'" + call->name + "' given " +
                     std::to_string(call->args.size()) + " argument(s)");
      return nullptr;
    }
    return call;
  }

  static std::unique_ptr<Node> MakeBinary(char op, std::unique_ptr<Node> lhs,
                                          std::unique_ptr<Node> rhs) {
    auto node = Make(Node::Kind::Binary, lhs->offset,
                     rhs->offset + rhs->length - lhs->offset);
    node->op = op;
    node->args.push_back(std::move(lhs));
    node->args.push_back(std::move(rhs));
    return node;
  }

  const std::vector<Token>& m_toks;
  ExprError* m_error;
  size_t m_pos = 0;
};

// ---------------------------------------------------------------- Evaluator

bool EvalCall(const Node& node, const std::vector<double>& args, double& out,
              ExprError* outError);

bool EvalNode(const Node& node, const SymbolTable& symbols, double& out,
              ExprError* outError) {
  const auto fail = [&](const std::string& message) {
    if (outError != nullptr) {
      *outError = {message, node.offset, node.length};
    }
    return false;
  };

  switch (node.kind) {
  case Node::Kind::Number:
    out = node.number;
    return true;
  case Node::Kind::Ident: {
    if (node.name == "pi") {
      out = angles::kPi;
      return true;
    }
    const auto it = symbols.find(node.name);
    if (it == symbols.end()) {
      return fail("unknown variable '" + node.name + "'");
    }
    out = it->second;
    return true;
  }
  case Node::Kind::Unary: {
    double v = 0.0;
    if (!EvalNode(*node.args[0], symbols, v, outError)) {
      return false;
    }
    out = -v;
    return true;
  }
  case Node::Kind::Binary: {
    double a = 0.0;
    double b = 0.0;
    if (!EvalNode(*node.args[0], symbols, a, outError) ||
        !EvalNode(*node.args[1], symbols, b, outError)) {
      return false;
    }
    switch (node.op) {
    case '+':
      out = a + b;
      break;
    case '-':
      out = a - b;
      break;
    case '*':
      out = a * b;
      break;
    case '/':
      if (b == 0.0) {
        return fail("division by zero");
      }
      out = a / b;
      break;
    case '^':
      out = std::pow(a, b);
      break;
    default:
      break;
    }
    break;
  }
  case Node::Kind::Call: {
    std::vector<double> args(node.args.size());
    for (size_t i = 0; i < node.args.size(); ++i) {
      if (!EvalNode(*node.args[i], symbols, args[i], outError)) {
        return false;
      }
    }
    if (!EvalCall(node, args, out, outError)) {
      return false;
    }
    break;
  }
  }
  if (!std::isfinite(out)) {
    return fail("result is not a finite number");
  }
  return true;
}

bool EvalCall(const Node& node, const std::vector<double>& args, double& out,
              ExprError* outError) {
  const std::string& f = node.name;
  if (f == "sin") {
    out = std::sin(angles::ToRadians(args[0]));
  } else if (f == "cos") {
    out = std::cos(angles::ToRadians(args[0]));
  } else if (f == "tan") {
    out = std::tan(angles::ToRadians(args[0]));
  } else if (f == "sqrt") {
    if (args[0] < 0.0) {
      if (outError != nullptr) {
        *outError = {"sqrt of a negative number", node.offset, node.length};
      }
      return false;
    }
    out = std::sqrt(args[0]);
  } else if (f == "abs") {
    out = std::fabs(args[0]);
  } else if (f == "floor") {
    out = std::floor(args[0]);
  } else if (f == "ceil") {
    out = std::ceil(args[0]);
  } else if (f == "min" || f == "max") {
    out = args[0];
    for (size_t i = 1; i < args.size(); ++i) {
      out = (f == "min") ? std::min(out, args[i]) : std::max(out, args[i]);
    }
  }
  return true;
}

void CollectDependencies(const Node& node, std::set<std::string>& out) {
  if (node.kind == Node::Kind::Ident && node.name != "pi") {
    out.insert(node.name);
  }
  for (const auto& child : node.args) {
    CollectDependencies(*child, out);
  }
}

class BuiltinCompiledExpr final : public CompiledExpr {
public:
  explicit BuiltinCompiledExpr(std::unique_ptr<Node> root)
      : m_root(std::move(root)) {
    CollectDependencies(*m_root, m_deps);
  }

  [[nodiscard]] const std::set<std::string>& Dependencies() const override {
    return m_deps;
  }

  std::expected<double, ExprError>
  Evaluate(const SymbolTable& symbols) const override {
    double value = 0.0;
    ExprError error;
    if (!EvalNode(*m_root, symbols, value, &error)) {
      return std::unexpected(error);
    }
    return value;
  }

private:
  std::unique_ptr<Node> m_root;
  std::set<std::string> m_deps;
};

} // namespace

// ----------------------------------------------------------------- Engines

std::expected<double, ExprError>
ExpressionEngine::EvaluateText(const std::string& text,
                               const SymbolTable& symbols) const {
  return Compile(text).and_then(
      [&symbols](const std::unique_ptr<CompiledExpr>& compiled) {
        return compiled->Evaluate(symbols);
      });
}

const ExpressionEngine& ExpressionEngine::Default() {
  static const BuiltinExpressionEngine engine;
  return engine;
}

std::expected<std::unique_ptr<CompiledExpr>, ExprError>
BuiltinExpressionEngine::Compile(const std::string& text) const {
  ExprError error;
  std::vector<Token> tokens;
  if (!Tokenize(text, tokens, &error)) {
    return std::unexpected(error);
  }
  auto root = Parser(tokens, &error).Parse();
  if (!root) {
    return std::unexpected(error);
  }
  return std::make_unique<BuiltinCompiledExpr>(std::move(root));
}

std::string
BuiltinExpressionEngine::RenameIdentifier(const std::string& text,
                                          const std::string& oldName,
                                          const std::string& newName) const {
  std::vector<Token> tokens;
  if (!Tokenize(text, tokens, nullptr)) {
    return text;
  }
  std::string result;
  result.reserve(text.size());
  size_t consumed = 0;
  for (const Token& tok : tokens) {
    if (tok.kind != TokKind::Ident || tok.text != oldName) {
      continue;
    }
    result.append(text, consumed, tok.offset - consumed);
    result += newName;
    consumed = tok.offset + tok.length;
  }
  result += text.substr(consumed);
  return result;
}

bool BuiltinExpressionEngine::IsValidParameterName(
    const std::string& name) const {
  // Unit names are reserved: a suffix binds to a preceding number regardless
  // of parameters, so a parameter literally named `mm`/`in`/`m`/… would be
  // unreachable in `10 <name>` and confusing everywhere else. (Existing docs
  // keep working — units only activate directly after a number token.)
  if (name.empty() || IsReservedName(name) || UnitFactor(name).has_value()) {
    return false;
  }
  if (std::isalpha(static_cast<unsigned char>(name[0])) == 0 &&
      name[0] != '_') {
    return false;
  }
  for (const char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '_') {
      return false;
    }
  }
  return true;
}
