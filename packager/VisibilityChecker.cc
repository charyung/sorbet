#include "packager/VisibilityChecker.h"
#include "ast/treemap/treemap.h"
#include "common/concurrency/ConcurrentQueue.h"
#include "common/formatting.h"
#include "common/sort.h"
#include "core/Context.h"
#include "core/errors/resolver.h"
#include <iterator>

namespace sorbet::packager {

namespace {

// For each __package.rb file, traverse the resolved tree and apply the visibility annotations to the symbols.
class PropagateVisibility final {
    const core::packages::PackageInfo &package;

    void recursiveExportSymbol(core::GlobalState &gs, bool firstSymbol, core::ClassOrModuleRef klass) {
        // There is a loop between a class and its singleton, and this is the easiest way to detect it.
        if (!firstSymbol && klass.data(gs)->flags.isExported) {
            return;
        }

        // We only mark symbols from this package.
        if (klass.data(gs)->loc().file().data(gs).getPackage() != this->package.mangledName()) {
            return;
        }

        klass.data(gs)->flags.isExported = true;

        for (const auto &child : klass.data(gs)->members()) {
            if (child.second.isClassOrModule()) {
                recursiveExportSymbol(gs, false, child.second.asClassOrModuleRef());
            } else if (child.second.isFieldOrStaticField()) {
                child.second.asFieldRef().data(gs)->flags.isExported = true;
            }
        }
    }

    void exportParentNamespace(core::GlobalState &gs, core::ClassOrModuleRef owner) {
        while (owner.exists() && !owner.data(gs)->flags.isExported) {
            owner.data(gs)->flags.isExported = true;
            owner = owner.data(gs)->owner;
        }
    }

    // Lookup the package name on the given root symbol, and mark the final symbol as exported.
    void exportRoot(core::GlobalState &gs, core::ClassOrModuleRef sym) {
        // Explicitly mark the symbol outside of the special package namespace as exported by this package, to allow
        // other packages to refer to this namespace. Ultimately it would be great to not have this, but it helps with a
        // migration to the visibility checker from the previous packages implementation.
        for (auto name : this->package.fullName()) {
            auto next = sym.data(gs)->findMember(gs, name);
            if (!next.exists() || !next.isClassOrModule()) {
                sym = core::Symbols::noClassOrModule();
                break;
            }

            sym = next.asClassOrModuleRef();
        }

        if (sym.exists()) {
            sym.data(gs)->flags.isExported = true;
        }
    }

    // Mark both the package and its test namespace as exported. This will terminate the recursive exporting in
    // `exportParentNamespace`, as it stops when it hits either the root or an exported symbol.
    void exportPackageRoots(core::GlobalState &gs) {
        this->exportRoot(gs, core::Symbols::root());

        auto test = core::Symbols::root().data(gs)->findMember(gs, core::Names::Constants::Test());
        if (test.exists() && test.isClassOrModule()) {
            this->exportRoot(gs, test.asClassOrModuleRef());
        }
    }

    void setPackageLocs(core::MutableContext ctx, core::LocOffsets loc, core::ClassOrModuleRef sym) {
        std::vector<core::NameRef> names;

        while (sym.exists() && sym != core::Symbols::PackageSpecRegistry()) {
            // The symbol isn't a package name if it's defined outside of the package registry.
            if (sym == core::Symbols::root()) {
                return;
            }

            names.emplace_back(sym.data(ctx)->name);
            sym = sym.data(ctx)->owner;
        }

        absl::c_reverse(names);

        {
            auto packageSym = core::Symbols::root();
            for (auto name : names) {
                auto member = packageSym.data(ctx)->findMember(ctx, name);
                if (!member.exists() || !member.isClassOrModule()) {
                    packageSym = core::Symbols::noClassOrModule();
                    break;
                }

                packageSym = member.asClassOrModuleRef();
            }

            if (packageSym.exists()) {
                packageSym.data(ctx)->addLoc(ctx, ctx.locAt(loc));
            }
        }

        {
            auto testSym = core::Symbols::root();
            auto member = testSym.data(ctx)->findMember(ctx, core::Names::Constants::Test());
            if (!member.exists() || !member.isClassOrModule()) {
                return;
            }

            testSym = member.asClassOrModuleRef();
            for (auto name : names) {
                auto member = testSym.data(ctx)->findMember(ctx, name);
                if (!member.exists() || !member.isClassOrModule()) {
                    testSym = core::Symbols::noClassOrModule();
                    break;
                }

                testSym = member.asClassOrModuleRef();
            }

            if (testSym.exists()) {
                testSym.data(ctx)->addLoc(ctx, ctx.locAt(loc));
            }
        }
    }

    void checkExportPackage(core::MutableContext &ctx, core::LocOffsets loc, core::NameRef litPackage) {
        if (litPackage != this->package.mangledName() && !this->package.importsPackage(litPackage)) {
            if (auto e = ctx.beginError(loc, core::errors::Resolver::InvalidExport)) {
                e.setHeader("Exporting a symbol that isn't owned by this package");
            }
        }
    }

    PropagateVisibility(const core::packages::PackageInfo &package) : package{package} {}

public:
    // Find uses of export and mark the symbols they mention as exported.
    ast::ExpressionPtr postTransformSend(core::MutableContext ctx, ast::ExpressionPtr tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        if (send.fun != core::Names::export_()) {
            return tree;
        }

        if (send.numPosArgs() != 1) {
            // an error will have been raised in the packager pass
            return tree;
        }

        auto *lit = ast::cast_tree<ast::ConstantLit>(send.getPosArg(0));
        if (lit == nullptr || lit->symbol == core::Symbols::StubModule()) {
            // We don't raise an explicit error here, as this is one of two cases:
            //   1. Export is given a non-constant argument
            //   2. The argument failed to resolve
            // In both cases, errors will be raised by previous passes.
            return tree;
        }

        if (lit->symbol.isClassOrModule()) {
            auto sym = lit->symbol.asClassOrModuleRef();
            checkExportPackage(ctx, send.loc, sym.data(ctx)->loc().file().data(ctx).getPackage());
            recursiveExportSymbol(ctx, true, sym);
            exportParentNamespace(ctx, sym.data(ctx)->owner);
        } else if (lit->symbol.isFieldOrStaticField()) {
            auto sym = lit->symbol.asFieldRef();
            checkExportPackage(ctx, send.loc, sym.data(ctx)->loc().file().data(ctx).getPackage());
            sym.data(ctx)->flags.isExported = true;
            exportParentNamespace(ctx, sym.data(ctx)->owner);
        }

        return tree;
    }

    ast::ExpressionPtr preTransformClassDef(core::MutableContext ctx, ast::ExpressionPtr tree) {
        auto &original = ast::cast_tree_nonnull<ast::ClassDef>(tree);

        if (original.symbol == core::Symbols::root()) {
            return tree;
        }

        setPackageLocs(ctx, original.name.loc(), original.symbol);

        return tree;
    }

    static ast::ParsedFile run(core::GlobalState &gs, ast::ParsedFile f) {
        if (!f.file.data(gs).isPackage()) {
            return f;
        }

        auto pkgName = f.file.data(gs).getPackage();
        if (!pkgName.exists()) {
            return f;
        }

        const auto &package = gs.packageDB().getPackageInfo(pkgName);
        if (!package.exists()) {
            return f;
        }

        PropagateVisibility pass{package};

        pass.exportPackageRoots(gs);

        core::MutableContext ctx{gs, core::Symbols::root(), f.file};
        f.tree = ast::TreeMap::apply(ctx, pass, std::move(f.tree));

        return f;
    }
};

class VisibilityCheckerPass final {
    bool nameMatchesPackage(const std::vector<core::NameRef> &revNameParts) {
        const auto &packageName = this->package.fullName();

        if (revNameParts.size() < packageName.size()) {
            return false;
        }

        return std::equal(packageName.begin(), packageName.end(), revNameParts.rbegin());
    }

public:
    const core::packages::PackageInfo &package;
    const bool insideTestFile;

    VisibilityCheckerPass(core::Context ctx, const core::packages::PackageInfo &package)
        : package{package}, insideTestFile{ctx.file.data(ctx).isPackagedTest()} {}

    // `keep-def` will reference constants in a way that looks like a packaging violation, but is actually fine. This
    // boolean allows for an early exit when we know we're in the context of processing one of these sends. Currently
    // the only sends that we process this way will not have any nested method calls, but if that changes this will need
    // to become a stack.
    bool ignoreConstant = false;

    ast::ExpressionPtr preTransformSend(core::Context ctx, ast::ExpressionPtr tree) {
        auto &send = ast::cast_tree_nonnull<ast::Send>(tree);
        this->ignoreConstant = send.fun == core::Names::keepForIde();
        return tree;
    }

    ast::ExpressionPtr postTransformSend(core::Context ctx, ast::ExpressionPtr tree) {
        this->ignoreConstant = false;
        return tree;
    }

    ast::ExpressionPtr postTransformConstantLit(core::Context ctx, ast::ExpressionPtr tree) {
        if (this->ignoreConstant) {
            return tree;
        }

        auto &lit = ast::cast_tree_nonnull<ast::ConstantLit>(tree);
        if (!lit.symbol.isClassOrModule() && !lit.symbol.isFieldOrStaticField()) {
            return tree;
        }

        auto loc = lit.symbol.loc(ctx);

        auto otherFile = loc.file();
        if (!otherFile.exists() || !otherFile.data(ctx).isPackaged()) {
            return tree;
        }

        // If the imported symbol comes from the test namespace, we must also be in the test namespace.
        if (otherFile.data(ctx).isPackagedTest() && !this->insideTestFile) {
            if (auto e = ctx.beginError(lit.loc, core::errors::Resolver::UsedTestOnlyName)) {
                e.setHeader("Used test-only constant `{}` in non-test file", lit.symbol.show(ctx));
            }
        }

        // no need to check visibility for these cases
        auto otherPackage = otherFile.data(ctx).getPackage();
        if (!otherPackage.exists() || this->package.mangledName() == otherPackage) {
            return tree;
        }

        // Did we resolve a packaged symbol in unpackaged code?
        if (!this->package.exists()) {
            if (auto e = ctx.beginError(lit.loc, core::errors::Resolver::PackagedSymbolInUnpackagedContext)) {
                e.setHeader("Packaged constant `{}` used in an unpackaged context", lit.symbol.show(ctx));
            }
            return tree;
        }

        // Did we fail to import the package that defines this symbol?
        auto importType = this->package.importsPackage(otherPackage);
        if (!importType.has_value()) {
            if (auto e = ctx.beginError(lit.loc, core::errors::Resolver::MissingImport)) {
                auto &pkg = ctx.state.packageDB().getPackageInfo(otherPackage);
                e.setHeader("Used constant `{}` from non-imported package `{}`", lit.symbol.show(ctx), pkg.show(ctx));
                if (auto exp = this->package.addImport(ctx, pkg, false)) {
                    e.addAutocorrect(std::move(exp.value()));
                }
            }
        }

        // Did we use a symbol from a `test_import` in a non-test context?
        if (*importType == core::packages::ImportType::Test && !this->insideTestFile) {
            if (auto e = ctx.beginError(lit.loc, core::errors::Resolver::UsedTestOnlyName)) {
                e.setHeader("Used `{}` constant `{}` in non-test file", "test_import", lit.symbol.show(ctx));
            }
        }

        bool isExported = false;
        if (lit.symbol.isClassOrModule()) {
            isExported = lit.symbol.asClassOrModuleRef().data(ctx)->flags.isExported;
        } else if (lit.symbol.isFieldOrStaticField()) {
            isExported = lit.symbol.asFieldRef().data(ctx)->flags.isExported;
        }

        // Did we use a constant that wasn't exported?
        if (!isExported) {
            if (auto e = ctx.beginError(lit.loc, core::errors::Resolver::UsedPackagePrivateName)) {
                auto &pkg = ctx.state.packageDB().getPackageInfo(otherPackage);
                e.setHeader("Package `{}` does not export `{}`", pkg.show(ctx), lit.symbol.show(ctx));
                if (auto exp = pkg.addExport(ctx, lit.symbol, false)) {
                    e.addAutocorrect(std::move(exp.value()));
                }
            }
        }

        return tree;
    }

    static std::vector<ast::ParsedFile> run(core::GlobalState &gs, WorkerPool &workers,
                                            std::vector<ast::ParsedFile> files) {
        Timer timeit(gs.tracer(), "packager.visibility_check");
        auto resultq = std::make_shared<BlockingBoundedQueue<ast::ParsedFile>>(files.size());
        auto fileq = std::make_shared<ConcurrentBoundedQueue<ast::ParsedFile>>(files.size());

        for (auto &file : files) {
            fileq->push(std::move(file), 1);
        }
        files.clear();

        workers.multiplexJob("VisibilityChecker", [&gs, fileq, resultq]() {
            ast::ParsedFile f;
            for (auto result = fileq->try_pop(f); !result.done(); result = fileq->try_pop(f)) {
                if (!f.file.data(gs).isPackage() && f.file.data(gs).isPackaged()) {
                    auto pkgName = f.file.data(gs).getPackage();
                    if (pkgName.exists()) {
                        core::Context ctx{gs, core::Symbols::root(), f.file};
                        VisibilityCheckerPass pass{ctx, gs.packageDB().getPackageInfo(pkgName)};
                        f.tree = ast::TreeMap::apply(ctx, pass, std::move(f.tree));
                    }
                }

                resultq->push(std::move(f), 1);
            }
        });

        ast::ParsedFile threadResult;
        for (auto result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), gs.tracer());
             !result.done();
             result = resultq->wait_pop_timed(threadResult, WorkerPool::BLOCK_INTERVAL(), gs.tracer())) {
            files.emplace_back(std::move(threadResult));
        }

        fast_sort(files, [](const auto &a, const auto &b) -> bool { return a.file < b.file; });

        return files;
    }
};

} // namespace

std::vector<ast::ParsedFile> VisibilityChecker::run(core::GlobalState &gs, WorkerPool &workers,
                                                    std::vector<ast::ParsedFile> files) {
    {
        Timer timeit(gs.tracer(), "packager.propagate_visibility");
        for (auto &f : files) {
            f = PropagateVisibility::run(gs, std::move(f));
        }
    }

    return VisibilityCheckerPass::run(gs, workers, std::move(files));
}

std::vector<ast::ParsedFile> VisibilityChecker::runIncremental(core::GlobalState &gs, WorkerPool &workers,
                                                               std::vector<ast::ParsedFile> files) {
    return VisibilityCheckerPass::run(gs, workers, std::move(files));
}

} // namespace sorbet::packager
