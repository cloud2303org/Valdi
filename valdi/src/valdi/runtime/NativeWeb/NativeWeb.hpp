#pragma once

#include "valdi/runtime/JavaScript/JavaScriptTypes.hpp"
#include "valdi/runtime/JavaScript/Modules/JavaScriptModuleFactory.hpp"

namespace Valdi {

/** Opt-in engine-neutral NSH Web binding. Module loading and shutdown require
 * the owning JS queue; release only retires an already-closed context. */
class NativeWeb {
  public:
    /** Finalize Rust attachments and clear JS handles before the VM is destroyed.
     * Idempotent; parent runtime must invoke this on the owning JS queue before
     * tearing down that queue. Retains a closed registry entry so late module
     * loads cannot reinstall, including after partial runtime teardown.
     */
    static void shutdown(IJavaScriptContext& context);

    /** Remove an already-closed context's registry entry, without calling Rust
     * or JS. Requires shutdown to have completed and no further JS to run.
     * Full teardown calls this just before VM destruction, after queue and
     * disposable cleanup. A partial runtime retains its closed entry until
     * destruction; after queue quiescence, its destruction thread may release
     * the entry. A reused context address can then install independently.
     */
    static void release(IJavaScriptContext& context);
};

/** Loading native_web/src/NativeWeb installs the Rust API and returns its
 * exports. Construction/registration alone does not change a context's globals.
 */
class NativeWebModuleFactory final : public JavaScriptModuleFactory {
  public:
    StringBox getModulePath() const final;
    JSValueRef loadModule(IJavaScriptContext& context, const ReferenceInfoBuilder& referenceInfo,
                          JSExceptionTracker& exceptionTracker) final;
};

} // namespace Valdi
