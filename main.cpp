#include <cstdint>
#include <cstdlib>
#include <memory>
#include <unordered_set>
#include <vector>

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
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

#ifndef CLANG_RESOURCE_DIR
#error "CLANG_RESOURCE_DIR must be defined by the build system."
#endif

using namespace clang::tooling;
using namespace llvm;

std::unordered_set<std::string> NOTABLE_FUNCTIONS = {
    "malloc",    "calloc",    "realloc",        "fork",          "fclose",
    "fflush",    "fread",     "fwrite",         "snprintf",      "strlcpy",
    "strlcat",   "vsnprintf", "write",          "send",          "pwrite",
    "sscanf",    "fscanf",    "posix_memalign", "aligned_alloc", "fsync",
    "fdatasync",
};

class ErrorCheckVisitor : public clang::RecursiveASTVisitor<ErrorCheckVisitor> {
public:
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

      auto &ctx = callExprCalleeDecl->getASTContext();
      if (!IsIgnoredCallStatement(callExpr, ctx)) {
        return RecursiveASTVisitor::TraverseStmt(S);
      }

      auto func = funcDecl->getNameAsString();
      if (NOTABLE_FUNCTIONS.find(func) == NOTABLE_FUNCTIONS.end()) {
        // not found.
        return RecursiveASTVisitor::TraverseStmt(S);
      }

      auto loc = callExpr->getExprLoc();
      auto presumedLoc = ctx.getSourceManager().getPresumedLoc(loc);

      // This is a notable function whose return value is ignored! Log it.
      llvm::outs() << "{\"name\":\"" << func << "\",\"filename\":\""
                   << presumedLoc.getFilename() << "\",\"line\":\""
                   << presumedLoc.getLine() << "\",\"column\":\""
                   << presumedLoc.getColumn()
                   << "\",\"handlingType\":\"ignored\"}\n";
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
};

class ErrorCheckConsumer : public clang::ASTConsumer {
public:
  virtual void HandleTranslationUnit(clang::ASTContext &Context) {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }

private:
  ErrorCheckVisitor Visitor;
};

class ErrorCheckAction : public clang::ASTFrontendAction {
public:
  virtual std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, StringRef) {
    return std::make_unique<ErrorCheckConsumer>();
  }
};

static cl::OptionCategory Category("errorck options");

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
    llvm::errs() << pRes.takeError();
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
  return Tool.run(newFrontendActionFactory<ErrorCheckAction>().get());
}
