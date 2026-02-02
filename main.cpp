#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
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

static bool LoadNotableFunctions(const std::string &path, NotableFunctions &out,
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
                             "id INTEGER PRIMARY KEY,"
                             "name TEXT NOT NULL,"
                             "filename TEXT NOT NULL,"
                             "line INTEGER NOT NULL,"
                             "column INTEGER NOT NULL,"
                             "handling_type TEXT NOT NULL"
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
        "handling_type) VALUES (?, ?, ?, ?, ?);";
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
                  const std::string &handling_type) {
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
                    SqliteWriter &writer)
      : notable_functions_(notable_functions), writer_(writer) {}

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
      bool ignored = false;
      switch (notable_it->second) {
      case ErrorReportingType::kReturnValue:
        ignored = IsIgnoredCallStatement(callExpr, ctx);
        break;
      case ErrorReportingType::kErrno:
        ignored = IsErrnoIgnored(callExpr, ctx);
        break;
      }
      if (!ignored) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }

      auto loc = callExpr->getExprLoc();
      auto presumedLoc = ctx.getSourceManager().getPresumedLoc(loc);
      std::string filename =
          presumedLoc.getFilename() ? presumedLoc.getFilename() : "";

      writer_.InsertCall(func, filename, presumedLoc.getLine(),
                         presumedLoc.getColumn(), "ignored");
    }

    return RecursiveASTVisitor::TraverseStmt(S);
  }

private:
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

      if (llvm::isa<clang::Expr>(ParentStmt)) {
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

  const NotableFunctions &notable_functions_;
  SqliteWriter &writer_;
};

class ErrorCheckConsumer : public clang::ASTConsumer {
public:
  ErrorCheckConsumer(const NotableFunctions &notable_functions,
                     SqliteWriter &writer)
      : Visitor(notable_functions, writer) {}

  virtual void HandleTranslationUnit(clang::ASTContext &Context) {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }

private:
  ErrorCheckVisitor Visitor;
};

class ErrorCheckAction : public clang::ASTFrontendAction {
public:
  ErrorCheckAction(const NotableFunctions &notable_functions,
                   SqliteWriter &writer)
      : notable_functions_(notable_functions), writer_(writer) {}

  virtual std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, StringRef) {
    return std::make_unique<ErrorCheckConsumer>(notable_functions_, writer_);
  }

private:
  const NotableFunctions &notable_functions_;
  SqliteWriter &writer_;
};

class ErrorCheckActionFactory : public clang::tooling::FrontendActionFactory {
public:
  ErrorCheckActionFactory(const NotableFunctions &notable_functions,
                          SqliteWriter &writer)
      : notable_functions_(notable_functions), writer_(writer) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    return std::make_unique<ErrorCheckAction>(notable_functions_, writer_);
  }

private:
  const NotableFunctions &notable_functions_;
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
  std::string error;
  if (!LoadNotableFunctions(NotableFunctionsPath, notable_functions, error)) {
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
  ErrorCheckActionFactory factory(notable_functions, writer);
  int result = Tool.run(&factory);
  if (!writer.ok()) {
    llvm::errs() << writer.error_message() << "\n";
    return EXIT_FAILURE;
  }
  return result;
}
