#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sqlite3.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#ifndef CLANG_RESOURCE_DIR
#error "CLANG_RESOURCE_DIR must be defined by the build system."
#endif

using namespace clang::tooling;
using namespace llvm;

static cl::OptionCategory Category("errorck options");

static cl::opt<std::string> NotableFunctionsPath(
    "notable-functions", cl::desc("Path to JSON array of functions to watch"),
    cl::value_desc("path"), cl::Required, cl::cat(Category));

static cl::opt<std::string>
    DatabasePath("db", cl::desc("Path to SQLite database output"),
                 cl::value_desc("path"), cl::Required, cl::cat(Category));

static cl::opt<bool>
    OverwriteIfNeeded("overwrite-if-needed",
                      cl::desc("Allow overwriting an existing database"),
                      cl::init(false), cl::cat(Category));

enum class ErrorReportingType {
  kReturnValue,
  kErrno,
};

using NotableFunctions = std::unordered_map<std::string, ErrorReportingType>;

struct AssignedLocation {
  std::string filename;
  unsigned line = 0;
  unsigned column = 0;
};

enum class HandlingType {
  kNone,
  kIgnored,
  kCastToVoid,
  kAssignedNotRead,
  kBranchedNoCatchall,
  kBranchedWithCatchall,
  kPropagated,
  kPassedToHandlerFn,
  kUsedOther,
  kLoggedNotHandled,
};

struct HandlingResult {
  HandlingType type = HandlingType::kNone;
  std::optional<AssignedLocation> assigned;
};

static bool ParseErrorReportingType(llvm::StringRef value,
                                    ErrorReportingType &out) {
  if (value == "return_value") {
    out = ErrorReportingType::kReturnValue;
    return true;
  }
  if (value == "errno") {
    out = ErrorReportingType::kErrno;
    return true;
  }
  return false;
}

static const char *HandlingTypeName(HandlingType type) {
  switch (type) {
  case HandlingType::kIgnored:
    return "ignored";
  case HandlingType::kCastToVoid:
    return "cast_to_void";
  case HandlingType::kAssignedNotRead:
    return "assigned_not_read";
  case HandlingType::kBranchedNoCatchall:
    return "branched_no_catchall";
  case HandlingType::kBranchedWithCatchall:
    return "branched_with_catchall";
  case HandlingType::kPropagated:
    return "propagated";
  case HandlingType::kPassedToHandlerFn:
    return "passed_to_handler_fn";
  case HandlingType::kUsedOther:
    return "used_other";
  case HandlingType::kLoggedNotHandled:
    return "logged_not_handled";
  case HandlingType::kNone:
    return "";
  }
  return "";
}

static bool LoadNotableFunctions(const std::string &path, NotableFunctions &out,
                                 std::unordered_set<std::string> &handlers,
                                 std::unordered_set<std::string> &loggers,
                                 std::string &error) {
  std::ifstream in(path);
  if (!in) {
    error = "Failed to open notable functions file: " + path;
    return false;
  }

  std::string contents((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  auto parsed = llvm::json::parse(contents);
  if (!parsed) {
    error = "Failed to parse notable functions JSON: " +
            llvm::toString(parsed.takeError());
    return false;
  }

  auto *array = parsed->getAsArray();
  if (!array) {
    error = "Notable functions JSON must be an array.";
    return false;
  }

  for (size_t i = 0; i < array->size(); ++i) {
    auto *object = (*array)[i].getAsObject();
    if (!object) {
      error = "Notable function entry at index " + std::to_string(i) +
              " must be an object.";
      return false;
    }

    auto name = object->getString("name");
    if (!name || name->empty()) {
      error = "Notable function entry at index " + std::to_string(i) +
              " must have a non-empty \"name\".";
      return false;
    }

    auto type = object->getString("type");
    if (type) {
      if (*type != "handler" && *type != "logger") {
        error = "Notable function entry at index " + std::to_string(i) +
                " has unsupported type \"" + type->str() + "\".";
        return false;
      }
      if (object->getString("reporting")) {
        error = "Notable function entry at index " + std::to_string(i) +
                " must not have a \"reporting\" field when using type "
                "\"handler\" or \"logger\".";
        return false;
      }
      if (out.find(name->str()) != out.end() ||
          handlers.find(name->str()) != handlers.end() ||
          loggers.find(name->str()) != loggers.end()) {
        error = "Duplicate notable function name: " + name->str();
        return false;
      }
      if (*type == "handler") {
        handlers.insert(name->str());
      } else {
        loggers.insert(name->str());
      }
      continue;
    }

    auto reporting = object->getString("reporting");
    if (!reporting) {
      error = "Notable function entry at index " + std::to_string(i) +
              " must have a \"reporting\" field.";
      return false;
    }

    ErrorReportingType reporting_type = ErrorReportingType::kReturnValue;
    if (!ParseErrorReportingType(*reporting, reporting_type)) {
      error = "Notable function entry at index " + std::to_string(i) +
              " has unsupported reporting type \"" + reporting->str() + "\".";
      return false;
    }

    if (handlers.find(name->str()) != handlers.end() ||
        loggers.find(name->str()) != loggers.end()) {
      error = "Duplicate notable function name: " + name->str();
      return false;
    }

    auto [it, inserted] = out.emplace(name->str(), reporting_type);
    if (!inserted) {
      error = "Duplicate notable function name: " + name->str();
      return false;
    }
  }

  return true;
}

static bool IsErrnoAccessorName(llvm::StringRef name) {
  return name == "__errno_location" || name == "__error";
}

static bool IsErrnoExpr(const clang::Expr *expr) {
  if (!expr) {
    return false;
  }

  expr = expr->IgnoreParenImpCasts();

  if (const auto *decl_ref = llvm::dyn_cast<clang::DeclRefExpr>(expr)) {
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(decl_ref->getDecl())) {
      return var->getName() == "errno";
    }
  }

  if (const auto *unary = llvm::dyn_cast<clang::UnaryOperator>(expr)) {
    if (unary->getOpcode() == clang::UO_Deref) {
      return IsErrnoExpr(unary->getSubExpr());
    }
  }

  if (const auto *call = llvm::dyn_cast<clang::CallExpr>(expr)) {
    if (const auto *callee = call->getDirectCallee()) {
      return IsErrnoAccessorName(callee->getName());
    }
  }

  return false;
}

// Visits a statement to see if "errno" or one of it's equivalent definitions
// is present in it. The result is stored in `found`.
class ErrnoReferenceVisitor
    : public clang::RecursiveASTVisitor<ErrnoReferenceVisitor> {
public:
  explicit ErrnoReferenceVisitor(bool &found) : found_(found) {}

  bool TraverseBinaryOperator(clang::BinaryOperator *op) {
    if (found_) {
      return false;
    }
    if (op->isAssignmentOp() && IsErrnoExpr(op->getLHS())) {
      return TraverseStmt(op->getRHS());
    }
    return RecursiveASTVisitor::TraverseBinaryOperator(op);
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *expr) {
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(expr->getDecl())) {
      if (var->getName() == "errno") {
        found_ = true;
      }
    }
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *expr) {
    if (const auto *callee = expr->getDirectCallee()) {
      if (IsErrnoAccessorName(callee->getName())) {
        found_ = true;
      }
    }
    return true;
  }

private:
  bool &found_;
};

static bool ContainsErrnoReference(const clang::Stmt *stmt) {
  if (!stmt) {
    return false;
  }

  bool found = false;
  ErrnoReferenceVisitor visitor(found);
  visitor.TraverseStmt(const_cast<clang::Stmt *>(stmt));
  return found;
}

static const clang::VarDecl *AsLocalVar(const clang::Expr *expr) {
  if (!expr) {
    return nullptr;
  }

  expr = expr->IgnoreParenImpCasts();
  const auto *decl_ref = llvm::dyn_cast<clang::DeclRefExpr>(expr);
  if (!decl_ref) {
    return nullptr;
  }

  const auto *var = llvm::dyn_cast<clang::VarDecl>(decl_ref->getDecl());
  if (!var || !var->hasLocalStorage()) {
    return nullptr;
  }

  return var;
}

static const clang::DeclRefExpr *DirectVarReference(const clang::Expr *expr,
                                                    const clang::VarDecl *var) {
  if (!expr || !var) {
    return nullptr;
  }

  expr = expr->IgnoreParenImpCasts();
  const auto *decl_ref = llvm::dyn_cast<clang::DeclRefExpr>(expr);
  if (!decl_ref || decl_ref->getDecl() != var) {
    return nullptr;
  }

  return decl_ref;
}

// Visits an expression to see if it references a specific variable passed by
// the caller and stores the result in `found_`.
class VarReferenceVisitor
    : public clang::RecursiveASTVisitor<VarReferenceVisitor> {
public:
  VarReferenceVisitor(const clang::VarDecl *var, bool &found)
      : var_(var), found_(found) {}

  bool VisitDeclRefExpr(clang::DeclRefExpr *expr) {
    if (expr->getDecl() == var_) {
      found_ = true;
      return false;
    }
    return true;
  }

private:
  const clang::VarDecl *var_;
  bool &found_;
};

static bool ContainsVarReference(const clang::Stmt *stmt,
                                 const clang::VarDecl *var) {
  if (!stmt || !var) {
    return false;
  }

  bool found = false;
  VarReferenceVisitor visitor(var, found);
  visitor.TraverseStmt(const_cast<clang::Stmt *>(stmt));
  return found;
}

// Visits a statement to see if it contains a particular call expression.
class CallReferenceVisitor
    : public clang::RecursiveASTVisitor<CallReferenceVisitor> {
public:
  CallReferenceVisitor(const clang::CallExpr *target, bool &found)
      : target_(target), found_(found) {}

  bool VisitCallExpr(clang::CallExpr *expr) {
    if (expr == target_) {
      found_ = true;
      return false;
    }
    return true;
  }

private:
  const clang::CallExpr *target_;
  bool &found_;
};

static bool ContainsCallReference(const clang::Stmt *stmt,
                                  const clang::CallExpr *call) {
  if (!stmt || !call) {
    return false;
  }

  bool found = false;
  CallReferenceVisitor visitor(call, found);
  visitor.TraverseStmt(const_cast<clang::Stmt *>(stmt));
  return found;
}

// Visits a statement to see if it returns a value containing a given variable.
class ReturnVarVisitor : public clang::RecursiveASTVisitor<ReturnVarVisitor> {
public:
  ReturnVarVisitor(const clang::VarDecl *var, bool &found)
      : var_(var), found_(found) {}

  bool VisitReturnStmt(clang::ReturnStmt *stmt) {
    if (found_) {
      return false;
    }
    const clang::Expr *value = stmt->getRetValue();
    if (value && ContainsVarReference(value, var_)) {
      found_ = true;
      return false;
    }
    return true;
  }

private:
  const clang::VarDecl *var_;
  bool &found_;
};

static bool ContainsReturnOfVar(const clang::Stmt *stmt,
                                const clang::VarDecl *var) {
  if (!stmt || !var) {
    return false;
  }
  bool found = false;
  ReturnVarVisitor visitor(var, found);
  visitor.TraverseStmt(const_cast<clang::Stmt *>(stmt));
  return found;
}

// Visits a statement to see if it returns a value containing errno.
class ReturnErrnoVisitor
    : public clang::RecursiveASTVisitor<ReturnErrnoVisitor> {
public:
  explicit ReturnErrnoVisitor(bool &found) : found_(found) {}

  bool VisitReturnStmt(clang::ReturnStmt *stmt) {
    if (found_) {
      return false;
    }
    const clang::Expr *value = stmt->getRetValue();
    if (value && ContainsErrnoReference(value)) {
      found_ = true;
      return false;
    }
    return true;
  }

private:
  bool &found_;
};

static bool ContainsReturnOfErrno(const clang::Stmt *stmt) {
  if (!stmt) {
    return false;
  }
  bool found = false;
  ReturnErrnoVisitor visitor(found);
  visitor.TraverseStmt(const_cast<clang::Stmt *>(stmt));
  return found;
}

static bool IsExplicitVoidCastExpr(const clang::Expr *expr) {
  if (!expr) {
    return false;
  }
  expr = expr->IgnoreParenImpCasts();
  const auto *cast = llvm::dyn_cast<clang::ExplicitCastExpr>(expr);
  return cast && cast->getType()->isVoidType();
}

static bool IsCallInSet(const clang::CallExpr *expr,
                        const std::unordered_set<std::string> &names) {
  if (!expr) {
    return false;
  }
  const auto *callee = expr->getDirectCallee();
  if (!callee) {
    return false;
  }
  return names.find(callee->getNameAsString()) != names.end();
}

struct VarUsageInfo {
  bool handler = false;
  bool logger = false;
  bool other = false;
};

// Visits an expression to record how a given variable is used, if at all.
class VarUsageVisitor : public clang::RecursiveASTVisitor<VarUsageVisitor> {
public:
  VarUsageVisitor(const clang::VarDecl *var,
                  const std::unordered_set<std::string> &handlers,
                  const std::unordered_set<std::string> &loggers,
                  VarUsageInfo &info)
      : var_(var), handlers_(handlers), loggers_(loggers), info_(info) {}

  bool TraverseCallExpr(clang::CallExpr *expr) {
    Context ctx = CurrentContext();
    Context arg_ctx = ctx;
    if (const auto *callee = expr->getDirectCallee()) {
      if (IsErrnoAccessorName(callee->getName())) {
        Mark();
      }
    }
    if (IsCallInSet(expr, handlers_)) {
      arg_ctx = Context::kHandler;
    } else if (IsCallInSet(expr, loggers_)) {
      arg_ctx = Context::kLogger;
    }

    if (auto *callee = expr->getCallee()) {
      PushContext(ctx);
      clang::RecursiveASTVisitor<VarUsageVisitor>::TraverseStmt(callee);
      PopContext();
    }
    for (auto *arg : expr->arguments()) {
      PushContext(arg_ctx);
      clang::RecursiveASTVisitor<VarUsageVisitor>::TraverseStmt(arg);
      PopContext();
    }
    return true;
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *expr) {
    if (expr->getDecl() == var_) {
      Mark();
    }
    return true;
  }

private:
  enum class Context {
    kOther,
    kLogger,
    kHandler,
  };

  Context CurrentContext() const {
    return context_stack_.empty() ? Context::kOther : context_stack_.back();
  }

  void PushContext(Context ctx) { context_stack_.push_back(ctx); }

  void PopContext() { context_stack_.pop_back(); }

  void Mark() {
    switch (CurrentContext()) {
    case Context::kHandler:
      info_.handler = true;
      break;
    case Context::kLogger:
      info_.logger = true;
      break;
    case Context::kOther:
      info_.other = true;
      break;
    }
  }

  const clang::VarDecl *var_;
  const std::unordered_set<std::string> &handlers_;
  const std::unordered_set<std::string> &loggers_;
  VarUsageInfo &info_;
  std::vector<Context> context_stack_;
};

static VarUsageInfo
AnalyzeVarUsage(const clang::Stmt *stmt, const clang::VarDecl *var,
                const std::unordered_set<std::string> &handlers,
                const std::unordered_set<std::string> &loggers) {
  VarUsageInfo info;
  if (!stmt || !var) {
    return info;
  }
  VarUsageVisitor visitor(var, handlers, loggers, info);
  visitor.TraverseStmt(const_cast<clang::Stmt *>(stmt));
  return info;
}

struct ErrnoUsageInfo {
  bool handler = false;
  bool logger = false;
  bool other = false;
};

// Visits an expression to record how errno is used, if at all.
class ErrnoUsageVisitor : public clang::RecursiveASTVisitor<ErrnoUsageVisitor> {
public:
  ErrnoUsageVisitor(const std::unordered_set<std::string> &handlers,
                    const std::unordered_set<std::string> &loggers,
                    ErrnoUsageInfo &info)
      : handlers_(handlers), loggers_(loggers), info_(info) {}

  bool TraverseBinaryOperator(clang::BinaryOperator *op) {
    if (op->isAssignmentOp() && IsErrnoExpr(op->getLHS())) {
      return TraverseStmt(op->getRHS());
    }
    return clang::RecursiveASTVisitor<
        ErrnoUsageVisitor>::TraverseBinaryOperator(op);
  }

  bool TraverseCallExpr(clang::CallExpr *expr) {
    Context ctx = CurrentContext();
    Context arg_ctx = ctx;
    if (const auto *callee = expr->getDirectCallee()) {
      if (IsErrnoAccessorName(callee->getName())) {
        Mark();
      }
    }
    if (IsCallInSet(expr, handlers_)) {
      arg_ctx = Context::kHandler;
    } else if (IsCallInSet(expr, loggers_)) {
      arg_ctx = Context::kLogger;
    }

    if (auto *callee = expr->getCallee()) {
      PushContext(ctx);
      clang::RecursiveASTVisitor<ErrnoUsageVisitor>::TraverseStmt(callee);
      PopContext();
    }
    for (auto *arg : expr->arguments()) {
      PushContext(arg_ctx);
      clang::RecursiveASTVisitor<ErrnoUsageVisitor>::TraverseStmt(arg);
      PopContext();
    }
    return true;
  }

  bool VisitDeclRefExpr(clang::DeclRefExpr *expr) {
    if (const auto *var = llvm::dyn_cast<clang::VarDecl>(expr->getDecl())) {
      if (var->getName() == "errno") {
        Mark();
      }
    }
    return true;
  }

  bool VisitCallExpr(clang::CallExpr *expr) {
    if (const auto *callee = expr->getDirectCallee()) {
      if (IsErrnoAccessorName(callee->getName())) {
        Mark();
      }
    }
    return true;
  }

private:
  enum class Context {
    kOther,
    kLogger,
    kHandler,
  };

  Context CurrentContext() const {
    return context_stack_.empty() ? Context::kOther : context_stack_.back();
  }

  void PushContext(Context ctx) { context_stack_.push_back(ctx); }

  void PopContext() { context_stack_.pop_back(); }

  void Mark() {
    switch (CurrentContext()) {
    case Context::kHandler:
      info_.handler = true;
      break;
    case Context::kLogger:
      info_.logger = true;
      break;
    case Context::kOther:
      info_.other = true;
      break;
    }
  }

  const std::unordered_set<std::string> &handlers_;
  const std::unordered_set<std::string> &loggers_;
  ErrnoUsageInfo &info_;
  std::vector<Context> context_stack_;
};

static ErrnoUsageInfo
AnalyzeErrnoUsage(const clang::Stmt *stmt,
                  const std::unordered_set<std::string> &handlers,
                  const std::unordered_set<std::string> &loggers) {
  ErrnoUsageInfo info;
  if (!stmt) {
    return info;
  }
  ErrnoUsageVisitor visitor(handlers, loggers, info);
  visitor.TraverseStmt(const_cast<clang::Stmt *>(stmt));
  return info;
}

class SqliteWriter {
public:
  ~SqliteWriter() {
    if (insert_stmt_) {
      sqlite3_finalize(insert_stmt_);
    }
    if (db_) {
      sqlite3_close(db_);
    }
  }

  bool Open(const std::string &path, bool overwrite, std::string &error) {
    std::error_code ec;
    std::filesystem::path db_path(path);
    bool exists = std::filesystem::exists(db_path, ec);
    if (ec) {
      error = "Failed to stat database path: " + path + ": " + ec.message();
      return false;
    }

    if (exists) {
      if (!overwrite) {
        error = "Database already exists: " + path;
        return false;
      }
      if (std::filesystem::is_directory(db_path, ec)) {
        error = "Database path is a directory: " + path;
        return false;
      }
      if (!std::filesystem::remove(db_path, ec)) {
        error = "Failed to remove existing database: " + path;
        if (ec) {
          error += ": " + ec.message();
        }
        return false;
      }
    }

    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
      error = "Failed to open database: " +
              std::string(db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc));
      if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      return false;
    }

    const char *schema_sql = "CREATE TABLE IF NOT EXISTS watched_calls ("
                             "    id INTEGER PRIMARY KEY,"
                             "    name TEXT NOT NULL,"
                             "    filename TEXT NOT NULL,"
                             "    line INTEGER NOT NULL,"
                             "    column INTEGER NOT NULL,"
                             "    handling_type TEXT NOT NULL,"
                             "    assigned_filename TEXT,"
                             "    assigned_line INTEGER,"
                             "    assigned_column INTEGER"
                             ");";
    char *errmsg = nullptr;
    rc = sqlite3_exec(db_, schema_sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
      error = "Failed to initialize schema: " +
              std::string(errmsg ? errmsg : sqlite3_errmsg(db_));
      sqlite3_free(errmsg);
      sqlite3_close(db_);
      db_ = nullptr;
      return false;
    }

    // Keep results deterministic when reusing a database path across runs.
    rc = sqlite3_exec(db_, "DELETE FROM watched_calls;", nullptr, nullptr,
                      &errmsg);
    if (rc != SQLITE_OK) {
      error = "Failed to clear watched_calls: " +
              std::string(errmsg ? errmsg : sqlite3_errmsg(db_));
      sqlite3_free(errmsg);
      sqlite3_close(db_);
      db_ = nullptr;
      return false;
    }

    const char *insert_sql =
        "INSERT INTO watched_calls (name, filename, line, column, "
        "handling_type, assigned_filename, assigned_line, assigned_column) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";
    rc = sqlite3_prepare_v2(db_, insert_sql, -1, &insert_stmt_, nullptr);
    if (rc != SQLITE_OK) {
      error = "Failed to prepare insert statement: " +
              std::string(sqlite3_errmsg(db_));
      sqlite3_close(db_);
      db_ = nullptr;
      insert_stmt_ = nullptr;
      return false;
    }

    return true;
  }

  bool InsertCall(const std::string &name, const std::string &filename,
                  unsigned line, unsigned column,
                  const std::string &handling_type,
                  const std::optional<AssignedLocation> &assigned) {
    if (!error_message_.empty()) {
      return false;
    }

    if (sqlite3_bind_text(insert_stmt_, 1, name.c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(insert_stmt_, 2, filename.c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int(insert_stmt_, 3, static_cast<int>(line)) !=
            SQLITE_OK ||
        sqlite3_bind_int(insert_stmt_, 4, static_cast<int>(column)) !=
            SQLITE_OK ||
        sqlite3_bind_text(insert_stmt_, 5, handling_type.c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
      SetError("Failed to bind insert parameters");
      sqlite3_reset(insert_stmt_);
      sqlite3_clear_bindings(insert_stmt_);
      return false;
    }

    if (assigned) {
      if (sqlite3_bind_text(insert_stmt_, 6, assigned->filename.c_str(), -1,
                            SQLITE_TRANSIENT) != SQLITE_OK ||
          sqlite3_bind_int(insert_stmt_, 7, static_cast<int>(assigned->line)) !=
              SQLITE_OK ||
          sqlite3_bind_int(insert_stmt_, 8,
                           static_cast<int>(assigned->column)) != SQLITE_OK) {
        SetError("Failed to bind assigned parameters");
        sqlite3_reset(insert_stmt_);
        sqlite3_clear_bindings(insert_stmt_);
        return false;
      }
    } else {
      if (sqlite3_bind_null(insert_stmt_, 6) != SQLITE_OK ||
          sqlite3_bind_null(insert_stmt_, 7) != SQLITE_OK ||
          sqlite3_bind_null(insert_stmt_, 8) != SQLITE_OK) {
        SetError("Failed to bind assigned parameters");
        sqlite3_reset(insert_stmt_);
        sqlite3_clear_bindings(insert_stmt_);
        return false;
      }
    }

    int rc = sqlite3_step(insert_stmt_);
    if (rc != SQLITE_DONE) {
      SetError("Failed to insert row");
      sqlite3_reset(insert_stmt_);
      sqlite3_clear_bindings(insert_stmt_);
      return false;
    }

    sqlite3_reset(insert_stmt_);
    sqlite3_clear_bindings(insert_stmt_);
    return true;
  }

  bool ok() const { return error_message_.empty(); }

  const std::string &error_message() const { return error_message_; }

private:
  void SetError(const std::string &message) {
    if (!error_message_.empty()) {
      return;
    }
    error_message_ = message + ": " + sqlite3_errmsg(db_);
  }

  sqlite3 *db_ = nullptr;
  sqlite3_stmt *insert_stmt_ = nullptr;
  std::string error_message_;
};

class ErrorCheckVisitor : public clang::RecursiveASTVisitor<ErrorCheckVisitor> {
public:
  ErrorCheckVisitor(const NotableFunctions &notable_functions,
                    const std::unordered_set<std::string> &handler_functions,
                    const std::unordered_set<std::string> &logger_functions,
                    SqliteWriter &writer)
      : notable_functions_(notable_functions),
        handler_functions_(handler_functions),
        logger_functions_(logger_functions), writer_(writer) {}

  bool TraverseStmt(clang::Stmt *S) {
    if (!S) {
      return true;
    }

    if (S->getStmtClass() == clang::Stmt::CallExprClass) {
      auto *callExpr = cast<clang::CallExpr>(S);
      auto *callExprCalleeDecl = callExpr->getCalleeDecl();
      if (!callExprCalleeDecl ||
          !callExprCalleeDecl->isFunctionOrFunctionTemplate()) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }

      auto *funcDecl = callExprCalleeDecl->getAsFunction();
      if (!funcDecl) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }

      auto func = funcDecl->getNameAsString();
      auto notable_it = notable_functions_.find(func);
      if (notable_it == notable_functions_.end()) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }

      auto &ctx = callExprCalleeDecl->getASTContext();
      HandlingResult handling;
      switch (notable_it->second) {
      case ErrorReportingType::kReturnValue:
        handling = AnalyzeReturnValue(callExpr, ctx);
        break;
      case ErrorReportingType::kErrno:
        handling = AnalyzeErrno(callExpr, ctx);
        break;
      }
      if (handling.type == HandlingType::kNone) {
        handling.type = HandlingType::kUsedOther;
      }

      auto loc = callExpr->getExprLoc();
      auto presumedLoc = ctx.getSourceManager().getPresumedLoc(loc);
      std::string filename =
          presumedLoc.getFilename() ? presumedLoc.getFilename() : "";
      writer_.InsertCall(func, filename, presumedLoc.getLine(),
                         presumedLoc.getColumn(),
                         HandlingTypeName(handling.type), handling.assigned);
    }

    return RecursiveASTVisitor::TraverseStmt(S);
  }

private:
  const clang::Expr *TopLevelExpr(const clang::Expr *expr,
                                  clang::ASTContext &ctx) const {
    if (!expr) {
      return nullptr;
    }
    const clang::Stmt *current = expr;
    const clang::Expr *top = expr;
    while (true) {
      auto parents = ctx.getParents(*current);
      if (parents.empty()) {
        return top;
      }
      if (const auto *parent_expr = parents[0].get<clang::Expr>()) {
        top = parent_expr;
        current = parent_expr;
        continue;
      }
      return top;
    }
  }

  bool IsTopLevelExplicitVoidCast(const clang::Expr *expr,
                                  clang::ASTContext &ctx) const {
    const clang::Expr *top = TopLevelExpr(expr, ctx);
    return top && IsExplicitVoidCastExpr(top);
  }

  bool IsReturnedCall(const clang::CallExpr *call_expr,
                      clang::ASTContext &ctx) const {
    if (!call_expr) {
      return false;
    }
    const clang::Stmt *current = call_expr;
    while (true) {
      auto parents = ctx.getParents(*current);
      if (parents.empty()) {
        return false;
      }
      if (const auto *parent_expr = parents[0].get<clang::Expr>()) {
        current = parent_expr;
        continue;
      }
      if (const auto *parent_stmt = parents[0].get<clang::Stmt>()) {
        const auto *return_stmt =
            llvm::dyn_cast<clang::ReturnStmt>(parent_stmt);
        if (!return_stmt) {
          return false;
        }
        const clang::Expr *value = return_stmt->getRetValue();
        return value && ContainsCallReference(value, call_expr);
      }
      return false;
    }
  }

  bool IsExplicitVoidCastStatement(const clang::Stmt *stmt,
                                   const clang::VarDecl *var) const {
    if (!stmt || !var) {
      return false;
    }
    const auto *expr = llvm::dyn_cast<clang::Expr>(stmt);
    if (!expr || !IsExplicitVoidCastExpr(expr)) {
      return false;
    }
    return ContainsVarReference(expr, var);
  }

  // We only want calls whose values are unused, so walk up through expression
  // wrappers and accept statement-position contexts.
  bool IsIgnoredCallStatement(const clang::CallExpr *CallExpr,
                              clang::ASTContext &Ctx) const {
    if (!CallExpr) {
      return false;
    }

    const clang::Stmt *Current = CallExpr;
    while (true) {
      auto Parents = Ctx.getParents(*Current);
      if (Parents.empty()) {
        return false;
      }

      const clang::Stmt *ParentStmt = Parents[0].get<clang::Stmt>();
      if (!ParentStmt) {
        return false;
      }

      if (const auto *parent_expr = llvm::dyn_cast<clang::Expr>(ParentStmt)) {
        bool is_wrapper =
            llvm::isa<clang::ParenExpr, clang::ImplicitCastExpr,
                      clang::ExplicitCastExpr, clang::ExprWithCleanups,
                      clang::CXXBindTemporaryExpr,
                      clang::MaterializeTemporaryExpr>(parent_expr);
        if (!is_wrapper) {
          return false;
        }
        Current = ParentStmt;
        continue;
      }

      if (llvm::isa<clang::CompoundStmt>(ParentStmt)) {
        return true;
      }

      if (const auto *IfStmt = llvm::dyn_cast<clang::IfStmt>(ParentStmt)) {
        return IfStmt->getThen() == Current || IfStmt->getElse() == Current;
      }

      if (const auto *WhileStmt =
              llvm::dyn_cast<clang::WhileStmt>(ParentStmt)) {
        return WhileStmt->getBody() == Current;
      }

      if (const auto *DoStmt = llvm::dyn_cast<clang::DoStmt>(ParentStmt)) {
        return DoStmt->getBody() == Current;
      }

      if (const auto *ForStmt = llvm::dyn_cast<clang::ForStmt>(ParentStmt)) {
        return ForStmt->getInit() == Current || ForStmt->getInc() == Current ||
               ForStmt->getBody() == Current;
      }

      if (const auto *SwitchStmt =
              llvm::dyn_cast<clang::SwitchStmt>(ParentStmt)) {
        return SwitchStmt->getBody() == Current;
      }

      if (const auto *CaseStmt = llvm::dyn_cast<clang::CaseStmt>(ParentStmt)) {
        return CaseStmt->getSubStmt() == Current;
      }

      if (const auto *DefaultStmt =
              llvm::dyn_cast<clang::DefaultStmt>(ParentStmt)) {
        return DefaultStmt->getSubStmt() == Current;
      }

      if (const auto *LabelStmt =
              llvm::dyn_cast<clang::LabelStmt>(ParentStmt)) {
        return LabelStmt->getSubStmt() == Current;
      }

      if (const auto *AttributedStmt =
              llvm::dyn_cast<clang::AttributedStmt>(ParentStmt)) {
        return AttributedStmt->getSubStmt() == Current;
      }

      return false;
    }
  }

  const clang::Stmt *FindStatementInCompound(const clang::Stmt *stmt,
                                             clang::ASTContext &ctx) const {
    if (!stmt) {
      return nullptr;
    }

    const clang::Stmt *current_stmt = stmt;
    const clang::Decl *current_decl = nullptr;

    while (true) {
      if (current_stmt) {
        auto parents = ctx.getParents(*current_stmt);
        if (parents.empty()) {
          return nullptr;
        }

        if (const auto *parent_stmt = parents[0].get<clang::Stmt>()) {
          if (llvm::isa<clang::CompoundStmt>(parent_stmt)) {
            return current_stmt;
          }
          current_stmt = parent_stmt;
          continue;
        }

        if (const auto *parent_decl = parents[0].get<clang::Decl>()) {
          current_decl = parent_decl;
          current_stmt = nullptr;
          continue;
        }

        return nullptr;
      }

      if (!current_decl) {
        return nullptr;
      }

      auto parents = ctx.getParents(*current_decl);
      if (parents.empty()) {
        return nullptr;
      }

      if (const auto *parent_stmt = parents[0].get<clang::Stmt>()) {
        current_stmt = parent_stmt;
        current_decl = nullptr;
        continue;
      }

      if (const auto *parent_decl = parents[0].get<clang::Decl>()) {
        current_decl = parent_decl;
        continue;
      }

      return nullptr;
    }
  }

  bool IsErrnoIgnored(const clang::CallExpr *call_expr,
                      clang::ASTContext &ctx) const {
    // Errno checks are typically adjacent to the call, so keep this local to
    // avoid pretending we have broader dataflow understanding.
    // TODO: Track errno usage across control flow to reduce false negatives.
    const clang::Stmt *statement = FindStatementInCompound(call_expr, ctx);
    if (!statement) {
      return true;
    }

    if (ContainsErrnoReference(statement)) {
      return false;
    }

    auto parents = ctx.getParents(*statement);
    if (parents.empty()) {
      return true;
    }

    const auto *compound = parents[0].get<clang::CompoundStmt>();
    if (!compound) {
      return true;
    }

    for (auto it = compound->body_begin(); it != compound->body_end(); ++it) {
      if (*it == statement) {
        auto next = it;
        ++next;
        if (next != compound->body_end() && ContainsErrnoReference(*next)) {
          return false;
        }
        break;
      }
    }

    return true;
  }

  enum class StatementUse {
    kNone,
    kPropagatedValue,
    kKilled,
    kLogged,
    kBranchedNoCatchall,
    kBranchedWithCatchall,
    kPassedToHandlerFn,
    kReturned,
    kCastToVoid,
    kUsedOther,
  };

  struct TrackingResult {
    HandlingType type = HandlingType::kNone;
    std::optional<clang::SourceLocation> assigned_loc;
  };

  std::optional<AssignedLocation>
  ToAssignedLocation(clang::SourceLocation loc, clang::ASTContext &ctx) const {
    if (!loc.isValid()) {
      return std::nullopt;
    }

    auto presumed = ctx.getSourceManager().getPresumedLoc(loc);
    if (presumed.isInvalid()) {
      return std::nullopt;
    }

    AssignedLocation assigned;
    assigned.filename = presumed.getFilename() ? presumed.getFilename() : "";
    assigned.line = presumed.getLine();
    assigned.column = presumed.getColumn();
    return assigned;
  }

  HandlingResult ToHandlingResult(const TrackingResult &tracked,
                                  clang::ASTContext &ctx) const {
    HandlingResult result;
    result.type = tracked.type;
    if (tracked.type == HandlingType::kAssignedNotRead &&
        tracked.assigned_loc) {
      result.assigned = ToAssignedLocation(*tracked.assigned_loc, ctx);
    }
    return result;
  }

  bool FindReturnValueAssignment(const clang::CallExpr *call_expr,
                                 clang::ASTContext &ctx,
                                 const clang::VarDecl *&out_var,
                                 const clang::Stmt *&out_stmt) const {
    const clang::Stmt *current = call_expr;
    while (true) {
      auto parents = ctx.getParents(*current);
      if (parents.empty()) {
        return false;
      }

      if (const auto *parent_expr = parents[0].get<clang::Expr>()) {
        current = parent_expr;
        continue;
      }

      if (const auto *parent_decl = parents[0].get<clang::Decl>()) {
        const auto *var = llvm::dyn_cast<clang::VarDecl>(parent_decl);
        if (!var || !var->hasLocalStorage()) {
          return false;
        }

        const clang::Expr *init = var->getInit();
        if (!init) {
          return false;
        }
        if (init->IgnoreParenImpCasts() !=
            llvm::cast<clang::Expr>(current)->IgnoreParenImpCasts()) {
          return false;
        }

        out_stmt = FindStatementInCompound(call_expr, ctx);
        if (!out_stmt || !llvm::isa<clang::DeclStmt>(out_stmt)) {
          return false;
        }

        out_var = var;
        return true;
      }

      if (const auto *parent_stmt = parents[0].get<clang::Stmt>()) {
        const auto *binop = llvm::dyn_cast<clang::BinaryOperator>(parent_stmt);
        if (!binop || binop->getOpcode() != clang::BO_Assign) {
          return false;
        }
        if (binop->getRHS()->IgnoreParenImpCasts() !=
            llvm::cast<clang::Expr>(current)->IgnoreParenImpCasts()) {
          return false;
        }

        const clang::VarDecl *lhs_var = AsLocalVar(binop->getLHS());
        if (!lhs_var) {
          return false;
        }

        out_stmt = FindStatementInCompound(binop, ctx);
        if (!out_stmt || !llvm::isa<clang::BinaryOperator>(out_stmt)) {
          return false;
        }

        out_var = lhs_var;
        return true;
      }

      return false;
    }
  }

  TrackingResult TrackReturnValue(const clang::CallExpr *call_expr,
                                  clang::ASTContext &ctx) const {
    const clang::VarDecl *assigned_var = nullptr;
    const clang::Stmt *assignment_stmt = nullptr;
    if (!FindReturnValueAssignment(call_expr, ctx, assigned_var,
                                   assignment_stmt)) {
      return {};
    }

    return TrackAssignedValue(assignment_stmt, ctx, assigned_var,
                              call_expr->getExprLoc(), true);
  }

  bool FindErrnoAssignmentInStatement(const clang::Stmt *stmt,
                                      const clang::VarDecl *&out_var,
                                      clang::SourceLocation &out_loc) const {
    if (!stmt) {
      return false;
    }

    if (const auto *decl_stmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
      for (const auto *decl : decl_stmt->decls()) {
        const auto *var = llvm::dyn_cast<clang::VarDecl>(decl);
        if (!var || !var->hasLocalStorage()) {
          continue;
        }
        const clang::Expr *init = var->getInit();
        if (!init) {
          continue;
        }
        const clang::Expr *unwrapped = init->IgnoreParenImpCasts();
        if (!IsErrnoExpr(unwrapped)) {
          continue;
        }
        out_var = var;
        out_loc = unwrapped->getExprLoc();
        return true;
      }
    }

    if (const auto *binop = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
      if (binop->getOpcode() != clang::BO_Assign) {
        return false;
      }
      const clang::VarDecl *lhs_var = AsLocalVar(binop->getLHS());
      if (!lhs_var) {
        return false;
      }
      const clang::Expr *rhs = binop->getRHS()->IgnoreParenImpCasts();
      if (!IsErrnoExpr(rhs)) {
        return false;
      }
      out_var = lhs_var;
      out_loc = rhs->getExprLoc();
      return true;
    }

    return false;
  }

  TrackingResult TrackErrnoAssignment(const clang::CallExpr *call_expr,
                                      clang::ASTContext &ctx) const {
    const clang::Stmt *call_stmt = FindStatementInCompound(call_expr, ctx);
    if (!call_stmt) {
      return {};
    }

    auto parents = ctx.getParents(*call_stmt);
    if (parents.empty()) {
      return {};
    }
    const auto *compound = parents[0].get<clang::CompoundStmt>();
    if (!compound) {
      return {};
    }

    const clang::VarDecl *assigned_var = nullptr;
    clang::SourceLocation assigned_loc;
    const clang::Stmt *assignment_stmt = nullptr;

    if (FindErrnoAssignmentInStatement(call_stmt, assigned_var, assigned_loc)) {
      assignment_stmt = call_stmt;
    } else {
      for (auto it = compound->body_begin(); it != compound->body_end(); ++it) {
        if (*it != call_stmt) {
          continue;
        }
        auto next = it;
        ++next;
        if (next == compound->body_end()) {
          break;
        }
        if (FindErrnoAssignmentInStatement(*next, assigned_var, assigned_loc)) {
          assignment_stmt = *next;
        }
        break;
      }
    }

    if (!assignment_stmt) {
      return {};
    }

    return TrackAssignedValue(assignment_stmt, ctx, assigned_var, assigned_loc,
                              false);
  }

  const clang::CallExpr *
  FindEnclosingCallWithArgument(const clang::CallExpr *call_expr,
                                clang::ASTContext &ctx) const {
    const clang::Stmt *current = call_expr;
    while (true) {
      auto parents = ctx.getParents(*current);
      if (parents.empty()) {
        return nullptr;
      }

      if (const auto *parent_call = parents[0].get<clang::CallExpr>()) {
        for (const auto *arg : parent_call->arguments()) {
          if (ContainsCallReference(arg, call_expr)) {
            return parent_call;
          }
        }
      }

      if (const auto *parent_stmt = parents[0].get<clang::Stmt>()) {
        current = parent_stmt;
        continue;
      }

      return nullptr;
    }
  }

  const clang::Stmt *NextStatementInCompound(const clang::Stmt *stmt,
                                             clang::ASTContext &ctx) const {
    if (!stmt) {
      return nullptr;
    }

    auto parents = ctx.getParents(*stmt);
    if (parents.empty()) {
      return nullptr;
    }

    const auto *compound = parents[0].get<clang::CompoundStmt>();
    if (!compound) {
      return nullptr;
    }

    for (auto it = compound->body_begin(); it != compound->body_end(); ++it) {
      if (*it == stmt) {
        auto next = it;
        ++next;
        if (next != compound->body_end()) {
          return *next;
        }
        break;
      }
    }

    return nullptr;
  }

  HandlingType BranchHandlingType(bool has_catchall) const {
    return has_catchall ? HandlingType::kBranchedWithCatchall
                        : HandlingType::kBranchedNoCatchall;
  }

  bool IfHasCatchall(const clang::IfStmt *stmt) const {
    const clang::IfStmt *current = stmt;
    while (current) {
      const clang::Stmt *else_stmt = current->getElse();
      if (!else_stmt) {
        return false;
      }
      if (const auto *else_if = llvm::dyn_cast<clang::IfStmt>(else_stmt)) {
        current = else_if;
        continue;
      }
      return true;
    }
    return false;
  }

  bool SwitchHasDefault(const clang::SwitchStmt *stmt) const {
    if (!stmt) {
      return false;
    }
    for (const auto *case_stmt = stmt->getSwitchCaseList(); case_stmt;
         case_stmt = case_stmt->getNextSwitchCase()) {
      if (llvm::isa<clang::DefaultStmt>(case_stmt)) {
        return true;
      }
    }
    return false;
  }

  std::optional<HandlingType>
  BranchHandlingForCall(const clang::CallExpr *call_expr,
                        clang::ASTContext &ctx) const {
    const clang::Stmt *statement = FindStatementInCompound(call_expr, ctx);
    if (!statement) {
      return std::nullopt;
    }

    if (const auto *if_stmt = llvm::dyn_cast<clang::IfStmt>(statement)) {
      if (ContainsCallReference(if_stmt->getCond(), call_expr)) {
        return BranchHandlingType(IfHasCatchall(if_stmt));
      }
    }

    if (const auto *switch_stmt =
            llvm::dyn_cast<clang::SwitchStmt>(statement)) {
      if (ContainsCallReference(switch_stmt->getCond(), call_expr)) {
        return BranchHandlingType(SwitchHasDefault(switch_stmt));
      }
    }

    return std::nullopt;
  }

  std::optional<HandlingType>
  BranchHandlingForVarCondition(const clang::Stmt *stmt,
                                const clang::VarDecl *var) const {
    if (!stmt || !var) {
      return std::nullopt;
    }

    if (const auto *if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
      if (ContainsVarReference(if_stmt->getCond(), var)) {
        return BranchHandlingType(IfHasCatchall(if_stmt));
      }
    }

    if (const auto *switch_stmt = llvm::dyn_cast<clang::SwitchStmt>(stmt)) {
      if (ContainsVarReference(switch_stmt->getCond(), var)) {
        return BranchHandlingType(SwitchHasDefault(switch_stmt));
      }
    }

    return std::nullopt;
  }

  std::optional<HandlingType>
  BranchHandlingForErrnoCondition(const clang::Stmt *stmt) const {
    if (!stmt) {
      return std::nullopt;
    }

    if (const auto *if_stmt = llvm::dyn_cast<clang::IfStmt>(stmt)) {
      if (ContainsErrnoReference(if_stmt->getCond())) {
        return BranchHandlingType(IfHasCatchall(if_stmt));
      }
    }

    if (const auto *switch_stmt = llvm::dyn_cast<clang::SwitchStmt>(stmt)) {
      if (ContainsErrnoReference(switch_stmt->getCond())) {
        return BranchHandlingType(SwitchHasDefault(switch_stmt));
      }
    }

    return std::nullopt;
  }

  HandlingType DirectHandlerLoggerUse(const clang::CallExpr *call_expr,
                                      clang::ASTContext &ctx) const {
    const clang::CallExpr *enclosing =
        FindEnclosingCallWithArgument(call_expr, ctx);
    if (!enclosing) {
      return HandlingType::kNone;
    }
    if (IsHandlerCall(enclosing)) {
      return HandlingType::kPassedToHandlerFn;
    }
    if (IsLoggerCall(enclosing)) {
      return HandlingType::kLoggedNotHandled;
    }
    return HandlingType::kNone;
  }

  HandlingType AnalyzeErrnoStatement(const clang::Stmt *stmt,
                                     bool &logged) const {
    if (!stmt) {
      return HandlingType::kNone;
    }

    ErrnoUsageInfo usage =
        AnalyzeErrnoUsage(stmt, handler_functions_, logger_functions_);
    if (usage.handler) {
      return HandlingType::kPassedToHandlerFn;
    }
    if (ContainsReturnOfErrno(stmt)) {
      return HandlingType::kPropagated;
    }
    if (auto branched = BranchHandlingForErrnoCondition(stmt)) {
      return *branched;
    }

    const clang::VarDecl *assigned_var = nullptr;
    clang::SourceLocation assigned_loc;
    if (FindErrnoAssignmentInStatement(stmt, assigned_var, assigned_loc)) {
      if (usage.logger) {
        logged = true;
      }
      return HandlingType::kNone;
    }

    if (usage.other) {
      return HandlingType::kUsedOther;
    }
    if (usage.logger) {
      logged = true;
    }
    return HandlingType::kNone;
  }

  HandlingResult MakeResult(HandlingType type) const {
    HandlingResult result;
    result.type = type;
    return result;
  }

  HandlingResult AnalyzeReturnValue(const clang::CallExpr *call_expr,
                                    clang::ASTContext &ctx) const {
    if (IsTopLevelExplicitVoidCast(call_expr, ctx)) {
      return MakeResult(HandlingType::kCastToVoid);
    }

    HandlingType direct = DirectHandlerLoggerUse(call_expr, ctx);
    if (direct != HandlingType::kNone) {
      return MakeResult(direct);
    }

    if (IsIgnoredCallStatement(call_expr, ctx)) {
      return MakeResult(HandlingType::kIgnored);
    }

    if (IsReturnedCall(call_expr, ctx)) {
      return MakeResult(HandlingType::kPropagated);
    }

    if (auto branched = BranchHandlingForCall(call_expr, ctx)) {
      return MakeResult(*branched);
    }

    HandlingResult tracked =
        ToHandlingResult(TrackReturnValue(call_expr, ctx), ctx);
    if (tracked.type != HandlingType::kNone) {
      return tracked;
    }

    return MakeResult(HandlingType::kUsedOther);
  }

  HandlingResult AnalyzeErrno(const clang::CallExpr *call_expr,
                              clang::ASTContext &ctx) const {
    if (IsErrnoIgnored(call_expr, ctx)) {
      return MakeResult(HandlingType::kIgnored);
    }

    const clang::Stmt *statement = FindStatementInCompound(call_expr, ctx);
    bool logged = false;
    HandlingType direct = AnalyzeErrnoStatement(statement, logged);
    if (direct != HandlingType::kNone) {
      return MakeResult(direct);
    }
    const clang::Stmt *next = NextStatementInCompound(statement, ctx);
    if (next) {
      direct = AnalyzeErrnoStatement(next, logged);
      if (direct != HandlingType::kNone) {
        return MakeResult(direct);
      }
    }

    HandlingResult tracked =
        ToHandlingResult(TrackErrnoAssignment(call_expr, ctx), ctx);
    if (tracked.type != HandlingType::kNone) {
      return tracked;
    }

    if (logged) {
      return MakeResult(HandlingType::kLoggedNotHandled);
    }

    return MakeResult(HandlingType::kUsedOther);
  }

  bool IsHandlerCall(const clang::CallExpr *call_expr) const {
    return IsCallInSet(call_expr, handler_functions_);
  }

  bool IsLoggerCall(const clang::CallExpr *call_expr) const {
    return IsCallInSet(call_expr, logger_functions_);
  }

  StatementUse AnalyzeStatementForVar(const clang::Stmt *stmt,
                                      const clang::VarDecl *var,
                                      const clang::VarDecl *&out_var,
                                      clang::SourceLocation &out_loc,
                                      bool allow_cast_to_void) const {
    if (!stmt || !var) {
      return StatementUse::kNone;
    }

    VarUsageInfo usage =
        AnalyzeVarUsage(stmt, var, handler_functions_, logger_functions_);
    if (usage.handler) {
      return StatementUse::kPassedToHandlerFn;
    }

    if (ContainsReturnOfVar(stmt, var)) {
      return StatementUse::kReturned;
    }

    if (auto branched = BranchHandlingForVarCondition(stmt, var)) {
      return *branched == HandlingType::kBranchedWithCatchall
                 ? StatementUse::kBranchedWithCatchall
                 : StatementUse::kBranchedNoCatchall;
    }

    if (const auto *decl_stmt = llvm::dyn_cast<clang::DeclStmt>(stmt)) {
      const clang::VarDecl *candidate = nullptr;
      clang::SourceLocation candidate_loc;
      for (const auto *decl : decl_stmt->decls()) {
        const auto *var_decl = llvm::dyn_cast<clang::VarDecl>(decl);
        if (!var_decl) {
          continue;
        }
        const clang::Expr *init = var_decl->getInit();
        if (!init) {
          continue;
        }
        if (!ContainsVarReference(init, var)) {
          continue;
        }
        const auto *direct = DirectVarReference(init, var);
        if (direct && var_decl->hasLocalStorage()) {
          if (candidate && candidate != var_decl) {
            return StatementUse::kUsedOther;
          }
          candidate = var_decl;
          candidate_loc = direct->getExprLoc();
          continue;
        }
        VarUsageInfo init_usage =
            AnalyzeVarUsage(init, var, handler_functions_, logger_functions_);
        if (init_usage.handler) {
          return StatementUse::kPassedToHandlerFn;
        }
        if (init_usage.other) {
          return StatementUse::kUsedOther;
        }
        if (init_usage.logger) {
          return StatementUse::kLogged;
        }
        return StatementUse::kUsedOther;
      }
      if (candidate) {
        out_var = candidate;
        out_loc = candidate_loc;
        return StatementUse::kPropagatedValue;
      }
    }

    if (const auto *binop = llvm::dyn_cast<clang::BinaryOperator>(stmt)) {
      if (binop->getOpcode() == clang::BO_Assign) {
        const clang::VarDecl *lhs_var = AsLocalVar(binop->getLHS());
        bool rhs_contains = ContainsVarReference(binop->getRHS(), var);
        if (lhs_var == var && !rhs_contains) {
          return StatementUse::kKilled;
        }
        if (rhs_contains) {
          const auto *direct = DirectVarReference(binop->getRHS(), var);
          if (direct && lhs_var && lhs_var != var) {
            out_var = lhs_var;
            out_loc = direct->getExprLoc();
            return StatementUse::kPropagatedValue;
          }
          VarUsageInfo rhs_usage = AnalyzeVarUsage(
              binop->getRHS(), var, handler_functions_, logger_functions_);
          if (rhs_usage.handler) {
            return StatementUse::kPassedToHandlerFn;
          }
          if (rhs_usage.other) {
            return StatementUse::kUsedOther;
          }
          if (rhs_usage.logger) {
            return StatementUse::kLogged;
          }
          return StatementUse::kUsedOther;
        }
      }
    }

    if (IsExplicitVoidCastStatement(stmt, var)) {
      return allow_cast_to_void ? StatementUse::kCastToVoid
                                : StatementUse::kUsedOther;
    }

    if (usage.other) {
      return StatementUse::kUsedOther;
    }
    if (usage.logger) {
      return StatementUse::kLogged;
    }

    return StatementUse::kNone;
  }

  TrackingResult TrackAssignedValue(const clang::Stmt *statement,
                                    clang::ASTContext &ctx,
                                    const clang::VarDecl *var,
                                    clang::SourceLocation assigned_loc,
                                    bool allow_cast_to_void) const {
    // Keep the scan local and linear so we don't imply dataflow across blocks.
    // TODO: Add control-flow-aware tracking so uses across branches aren't
    // misclassified as unread.
    if (!statement || !var) {
      return {};
    }

    auto parents = ctx.getParents(*statement);
    if (parents.empty()) {
      return {};
    }

    const auto *compound = parents[0].get<clang::CompoundStmt>();
    if (!compound) {
      return {};
    }

    const clang::VarDecl *current_var = var;
    clang::SourceLocation current_loc = assigned_loc;
    bool logged = false;
    bool found = false;
    for (auto it = compound->body_begin(); it != compound->body_end(); ++it) {
      if (!found) {
        if (*it == statement) {
          found = true;
        }
        continue;
      }

      const clang::VarDecl *next_var = nullptr;
      clang::SourceLocation next_loc;
      switch (AnalyzeStatementForVar(*it, current_var, next_var, next_loc,
                                     allow_cast_to_void)) {
      case StatementUse::kNone:
        break;
      case StatementUse::kLogged:
        logged = true;
        break;
      case StatementUse::kBranchedNoCatchall: {
        TrackingResult result;
        result.type = HandlingType::kBranchedNoCatchall;
        return result;
      }
      case StatementUse::kBranchedWithCatchall: {
        TrackingResult result;
        result.type = HandlingType::kBranchedWithCatchall;
        return result;
      }
      case StatementUse::kPassedToHandlerFn: {
        TrackingResult result;
        result.type = HandlingType::kPassedToHandlerFn;
        return result;
      }
      case StatementUse::kReturned: {
        TrackingResult result;
        result.type = HandlingType::kPropagated;
        return result;
      }
      case StatementUse::kCastToVoid: {
        TrackingResult result;
        result.type = HandlingType::kCastToVoid;
        return result;
      }
      case StatementUse::kUsedOther: {
        TrackingResult result;
        result.type = HandlingType::kUsedOther;
        return result;
      }
      case StatementUse::kPropagatedValue:
        current_var = next_var;
        current_loc = next_loc;
        break;
      case StatementUse::kKilled:
        if (logged) {
          TrackingResult result;
          result.type = HandlingType::kLoggedNotHandled;
          return result;
        } else {
          TrackingResult result;
          result.type = HandlingType::kAssignedNotRead;
          result.assigned_loc = current_loc;
          return result;
        }
      }
    }

    if (logged) {
      TrackingResult result;
      result.type = HandlingType::kLoggedNotHandled;
      return result;
    }
    TrackingResult result;
    result.type = HandlingType::kAssignedNotRead;
    result.assigned_loc = current_loc;
    return result;
  }

  const NotableFunctions &notable_functions_;
  const std::unordered_set<std::string> &handler_functions_;
  const std::unordered_set<std::string> &logger_functions_;
  SqliteWriter &writer_;
};

class ErrorCheckConsumer : public clang::ASTConsumer {
public:
  ErrorCheckConsumer(const NotableFunctions &notable_functions,
                     const std::unordered_set<std::string> &handler_functions,
                     const std::unordered_set<std::string> &logger_functions,
                     SqliteWriter &writer)
      : Visitor(notable_functions, handler_functions, logger_functions,
                writer) {}

  virtual void HandleTranslationUnit(clang::ASTContext &Context) {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }

private:
  ErrorCheckVisitor Visitor;
};

class ErrorCheckAction : public clang::ASTFrontendAction {
public:
  ErrorCheckAction(const NotableFunctions &notable_functions,
                   const std::unordered_set<std::string> &handler_functions,
                   const std::unordered_set<std::string> &logger_functions,
                   SqliteWriter &writer)
      : notable_functions_(notable_functions),
        handler_functions_(handler_functions),
        logger_functions_(logger_functions), writer_(writer) {}

  virtual std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, StringRef) {
    return std::make_unique<ErrorCheckConsumer>(
        notable_functions_, handler_functions_, logger_functions_, writer_);
  }

private:
  const NotableFunctions &notable_functions_;
  const std::unordered_set<std::string> &handler_functions_;
  const std::unordered_set<std::string> &logger_functions_;
  SqliteWriter &writer_;
};

class ErrorCheckActionFactory : public clang::tooling::FrontendActionFactory {
public:
  ErrorCheckActionFactory(
      const NotableFunctions &notable_functions,
      const std::unordered_set<std::string> &handler_functions,
      const std::unordered_set<std::string> &logger_functions,
      SqliteWriter &writer)
      : notable_functions_(notable_functions),
        handler_functions_(handler_functions),
        logger_functions_(logger_functions), writer_(writer) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<ErrorCheckAction>(
        notable_functions_, handler_functions_, logger_functions_, writer_);
  }

private:
  const NotableFunctions &notable_functions_;
  const std::unordered_set<std::string> &handler_functions_;
  const std::unordered_set<std::string> &logger_functions_;
  SqliteWriter &writer_;
};

// The main function just initializes and drives libTooling. Most of the work
// is done in the various classes defined in this file.
//
// The main work is done by defining a FrontendAction, which as the name
// implies defines an action done by the frontend (clang). In our case, we just
// want to analyze the AST, so we just delegate to our ASTConsumer,
// ErrorCheckConsumer.
//
// An ASTConsumer is a class which consumes an AST, and does various things
// with it. Again, all we want to do is analyze it and not actually modify it
// or write to anything so we just delegate to our RecursiveASTVisitor, which
// allows running some code whenever certain AST nodes are visited by clang.
int main(int argc, const char **argv) {
  auto pRes = CommonOptionsParser::create(argc, argv, Category);
  if (!pRes) {
    llvm::logAllUnhandledErrors(pRes.takeError(), llvm::errs());
    return EXIT_FAILURE;
  }

  NotableFunctions notable_functions;
  std::unordered_set<std::string> handler_functions;
  std::unordered_set<std::string> logger_functions;
  std::string error;
  if (!LoadNotableFunctions(NotableFunctionsPath, notable_functions,
                            handler_functions, logger_functions, error)) {
    llvm::errs() << error << "\n";
    return EXIT_FAILURE;
  }

  SqliteWriter writer;
  if (!writer.Open(DatabasePath, OverwriteIfNeeded, error)) {
    llvm::errs() << error << "\n";
    return EXIT_FAILURE;
  }

  CommonOptionsParser &OptionsParser = pRes.get();
  std::vector<std::string> SourcePaths = OptionsParser.getSourcePathList();
  if (SourcePaths.empty()) {
    SourcePaths = OptionsParser.getCompilations().getAllFiles();
  }
  ClangTool Tool(OptionsParser.getCompilations(), SourcePaths);
  const std::string ResourceDir = CLANG_RESOURCE_DIR;
  if (!ResourceDir.empty()) {
    // Ensure builtin headers come from the LLVM install, not the host
    // toolchain. This is to prevent the "cannot find stddef.h" errors.
    const std::string ResourceArg = "-resource-dir=" + ResourceDir;
    Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
        ResourceArg.c_str(), ArgumentInsertPosition::BEGIN));
  }
  ErrorCheckActionFactory factory(notable_functions, handler_functions,
                                  logger_functions, writer);
  int result = Tool.run(&factory);
  if (!writer.ok()) {
    llvm::errs() << writer.error_message() << "\n";
    return EXIT_FAILURE;
  }
  return result;
}
