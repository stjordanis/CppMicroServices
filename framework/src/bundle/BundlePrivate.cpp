/*=============================================================================

  Library: CppMicroServices

  Copyright (c) The CppMicroServices developers. See the COPYRIGHT
  file at the top-level directory of this distribution and at
  https://github.com/saschazelzer/CppMicroServices/COPYRIGHT .

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

=============================================================================*/

#include "BundlePrivate.h"

#include "cppmicroservices/Bundle.h"
#include "cppmicroservices/BundleActivator.h"
#include "cppmicroservices/BundleContext.h"
#include "cppmicroservices/BundleResource.h"
#include "cppmicroservices/BundleResourceStream.h"
#include "cppmicroservices/Framework.h"
#include "cppmicroservices/FrameworkEvent.h"
#include "cppmicroservices/ServiceRegistration.h"

#include "BundleArchive_p.h"
#include "BundleContextPrivate.h"
#include "BundleResourceContainer_p.h"
#include "BundleThread_p.h"
#include "BundleUtils_p.h"
#include "CoreBundleContext_p.h"
#include "Fragment_p.h"
#include "ServiceReferenceBasePrivate.h"
#include "Utils_p.h" // cppmicroservices::ToString()

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>

namespace cppmicroservices {

Bundle MakeBundle(const std::shared_ptr<BundlePrivate>& d)
{
  return Bundle(d);
}

void BundlePrivate::Stop(uint32_t options)
{
  std::exception_ptr savedException;

  {
    auto l = coreCtx->resolver.Lock();
    if (IsFragment())
    {
      throw std::runtime_error("Bundle#" + cppmicroservices::ToString(id) + ", can not stop a fragment");
    }

    // 1:
    if (state == Bundle::STATE_UNINSTALLED)
    {
      throw std::logic_error("Bundle is uninstalled");
    }

    // 2: If an operation is in progress, wait a little
    WaitOnOperation(coreCtx->resolver, l, "Bundle::Stop", false);

    // 3:
    if ((options & Bundle::STOP_TRANSIENT) == 0)
    {
      SetAutostartSetting(-1);
    }
    switch (static_cast<Bundle::State>(state.load()))
    {
    case Bundle::STATE_INSTALLED:
    case Bundle::STATE_RESOLVED:
    case Bundle::STATE_STOPPING:
    case Bundle::STATE_UNINSTALLED:
      // 4:
      return;

    case Bundle::STATE_ACTIVE:
    case Bundle::STATE_STARTING: // Lazy start...
      savedException = Stop0(l);
      break;
    }
  }

  if (savedException)
  {
    std::rethrow_exception(savedException);
  }
}

std::exception_ptr BundlePrivate::Stop0(UniqueLock& resolveLock)
{
  wasStarted = state == Bundle::STATE_ACTIVE;
  // 5:
  state = Bundle::STATE_STOPPING;
  operation = OP_DEACTIVATING;
  // 6-13:
  std::exception_ptr savedException = GetBundleThread()->CallStop1(this, resolveLock);
  if (state != Bundle::STATE_UNINSTALLED)
  {
    state = Bundle::STATE_RESOLVED;
    GetBundleThread()->BundleChanged(BundleEvent(BundleEvent::BUNDLE_STOPPED, MakeBundle(this->shared_from_this())), resolveLock);
    coreCtx->resolver.NotifyAll();
    operation = OP_IDLE;
  }
  return savedException;
}

std::exception_ptr BundlePrivate::Stop1()
{
  std::exception_ptr res;

  // 6:
  coreCtx->listeners.BundleChanged(BundleEvent(BundleEvent::BUNDLE_STOPPING, MakeBundle(this->shared_from_this())));

  // 7:
  if (wasStarted && bactivator != nullptr)
  {
    try
    {
      bactivator->Stop(MakeBundleContext(bundleContext.Load()));
    }
    catch (const std::exception& e)
    {
      res = std::make_exception_ptr(
            std::runtime_error("Bundle#" + cppmicroservices::ToString(id) + ", BundleActivator::Stop() failed:" + e.what()));
    }

    // if stop was aborted (uninstall or timeout), make sure
    // FinalizeActivation() has finished before checking aborted/state
    {
      auto l = coreCtx->resolver.Lock(); US_UNUSED(l);
      std::string cause;
      if (aborted == static_cast<uint8_t>(Aborted::YES))
      {
        if (Bundle::STATE_UNINSTALLED == state)
        {
          cause = "Bundle uninstalled during Stop()";
        }
        else
        {
          cause = "Bundle activator Stop() time-out";
        }
      }
      else
      {
        aborted = static_cast<uint8_t>(Aborted::NO); // signal to other thread that BundleThread
                               // concludes stop
        if (Bundle::STATE_STOPPING != state)
        {
          cause = "Bundle changed state because of refresh during Stop()";
        }
      }
      if (!cause.empty())
      {
        res = std::make_exception_ptr(std::runtime_error("Bundle stop failed: " + cause));
      }
    }
    bactivator = nullptr;
  }

  if (operation == OP_DEACTIVATING)
  {
    Stop2();
  }

  return res;
}

void BundlePrivate::Stop2()
{
  // Call hooks after we've called BundleActivator::Stop(), but before we've
  // cleared all resources
  std::shared_ptr<BundleContextPrivate> ctx = bundleContext.Load();
  if (ctx)
  {
    coreCtx->listeners.HooksBundleStopped(ctx);
    // 8-10:
    RemoveBundleResources();
    ctx->Invalidate();
    bundleContext.Store(std::shared_ptr<BundleContextPrivate>());
  }
}

void BundlePrivate::WaitOnOperation(WaitConditionType& wc, LockType& lock, const std::string& src, bool longWait)
{
  if (operation.load() != OP_IDLE)
  {
    std::chrono::milliseconds waitfor = longWait ? std::chrono::milliseconds(20000) : std::chrono::milliseconds(500);
    if (wc.WaitFor(lock, waitfor, [this]{ return operation.load() == OP_IDLE; }))
    {
      return;
    }

    std::string op;
    switch (operation.load())
    {
    case OP_IDLE:
      // Should not happen!
      return;
    case OP_ACTIVATING:
      op = "start";
      break;
    case OP_DEACTIVATING:
      op = "stop";
      break;
    case OP_RESOLVING:
      op = "resolve";
      break;
    case OP_UNINSTALLING:
      op = "uninstall";
      break;
    case OP_UNRESOLVING:
      op = "unresolve";
      break;
    case OP_UPDATING:
      op = "update";
      break;
    }
    throw std::runtime_error(src + " called during " + op + " of Bundle#" + cppmicroservices::ToString(id));
  }
}

Bundle::State BundlePrivate::GetUpdatedState(BundlePrivate* trigger, LockType& l)
{
  if (state == Bundle::STATE_INSTALLED)
  {
    try
    {
      WaitOnOperation(coreCtx->resolver, l, "Bundle.resolve", true);
      if (state == Bundle::STATE_INSTALLED)
      {
        if (trigger != nullptr)
        {
          coreCtx->resolverHooks.BeginResolve(trigger);
        }
        if (IsFragment())
        {
          for (auto host : fragment->Targets())
          {
            if (host->state == Bundle::STATE_INSTALLED)
            {
              // Try resolve our host
              // NYI! Detect circular attach
              host->GetUpdatedState(nullptr, l);
            }
            else
            {
              if (!fragment->IsHost(host))
              {
                // TODO: attach to host
                //AttachToFragmentHost(host);
              }
            }
          }

          if (state == Bundle::STATE_INSTALLED && fragment->HasHosts())
          {
            // TODO wiring
            // SetWired();
            state = Bundle::STATE_RESOLVED;
            operation = OP_RESOLVING;
            GetBundleThread()->BundleChanged(BundleEvent(BundleEvent::BUNDLE_RESOLVED, MakeBundle(this->shared_from_this())), l);
            operation = OP_IDLE;
          }
        }
        else
        {
          // TDOD resolve
          if (true) // ResolvePackages(trigger))
          {
            // TODO wiring
            // SetWired();
            state = Bundle::STATE_RESOLVED;
            operation = OP_RESOLVING;
            // update state of fragments
            for (BundlePrivate* f : fragments)
            {
              f->GetUpdatedState(nullptr, l);
            }
            GetBundleThread()->BundleChanged(BundleEvent(BundleEvent::BUNDLE_RESOLVED, MakeBundle(this->shared_from_this())), l);
            operation = OP_IDLE;
          }
          else
          {
            // std::string reason = GetResolveFailReason();
            //throw std::runtime_error("Bundle#" + cppmicroservices::ToString(info.id) + ", unable to resolve: "
            //                          + reason);
          }
        }
        if (trigger != nullptr)
        {
          coreCtx->resolverHooks.EndResolve(trigger);
        }
      }
    }
    catch (const std::exception& )
    {
      resolveFailException = std::current_exception();
      coreCtx->listeners.SendFrameworkEvent(FrameworkEvent(FrameworkEvent::Type::FRAMEWORK_ERROR, MakeBundle(this->shared_from_this()), std::string(), std::current_exception()));

      if (trigger != nullptr)
      {
        try
        {
          coreCtx->resolverHooks.EndResolve(trigger);
        }
        catch (const std::exception& )
        {
          resolveFailException = std::current_exception();
          coreCtx->listeners.SendFrameworkEvent(FrameworkEvent(FrameworkEvent::Type::FRAMEWORK_ERROR, MakeBundle(this->shared_from_this()), std::string(), std::current_exception()));
        }
      }
    }
  }
  return static_cast<Bundle::State>(state.load());
}

void BundlePrivate::SetStateInstalled(bool sendEvent, UniqueLock& resolveLock)
{
  // Make sure that bundleContext is invalid
  std::shared_ptr<BundleContextPrivate> ctx;
  if ((ctx = bundleContext.Exchange(ctx)))
  {
    ctx->Invalidate();
  }
  if (IsFragment())
  {
    fragment->RemoveHost(nullptr);
  }
  // TODO Wiring
  //ClearWiring();
  state = Bundle::STATE_INSTALLED;
  if (sendEvent) {
    operation = OP_UNRESOLVING;
    GetBundleThread()->BundleChanged(BundleEvent(BundleEvent::BUNDLE_UNRESOLVED, MakeBundle(this->shared_from_this())), resolveLock);
  }
  operation = OP_IDLE;
}

void BundlePrivate::FinalizeActivation(LockType& l)
{
  // 4: Resolve bundle (if needed)
  switch (GetUpdatedState(this, l))
  {
  case Bundle::STATE_INSTALLED:
  {
    std::rethrow_exception(resolveFailException);
  }
  case Bundle::STATE_STARTING:
  {
    if (operation == OP_ACTIVATING)
    {
      // finalization already in progress.
      return;
    }
    // Lazy activation; fall through to STATE_RESOLVED.
  }
  case Bundle::STATE_RESOLVED:
  {
    // 6:
    state = Bundle::STATE_STARTING;
    operation = OP_ACTIVATING;
    if (coreCtx->debug.lazyActivation)
    {
      DIAG_LOG(*coreCtx->sink) << "activating #" << id;
    }
    // 7:
    std::shared_ptr<BundleContextPrivate> null_expected;
    std::shared_ptr<BundleContextPrivate> ctx(new BundleContextPrivate(this));
    bundleContext.CompareExchange(null_expected, ctx);
    auto e = GetBundleThread()->CallStart0(this, l);
    operation = OP_IDLE;
    coreCtx->resolver.NotifyAll();
    if (e)
    {
      std::rethrow_exception(e);
    }
    break;
  }
  case Bundle::STATE_ACTIVE:
    break;
  case Bundle::STATE_STOPPING:
    // This happens if call start from inside the BundleActivator.stop
    // method.
    // Don't allow it.
    throw std::runtime_error("Bundle#" + cppmicroservices::ToString(id) +
                             ", start called from BundleActivator::Stop");
  case Bundle::STATE_UNINSTALLED:
    throw std::logic_error("Bundle is in UNINSTALLED state");
  }
}

void BundlePrivate::Start(uint32_t options)
{
  {
    auto l = coreCtx->resolver.Lock();
    if (state == Bundle::STATE_UNINSTALLED)
    {
      throw std::logic_error("Bundle is uninstalled");
    }
    coreCtx->resolverHooks.CheckResolveBlocked();

    // Initialize the activation; checks initialization of lazy
    // activation.

    // 1: If an operation is in progress, wait a little
    WaitOnOperation(coreCtx->resolver, l, "Bundle::Start", false);

    // 2: Start() is idempotent, i.e., nothing to do when already started
    if (state == Bundle::STATE_ACTIVE)
    {
      return;
    }

    // 3: Record non-transient start requests.
    if ((options & Bundle::START_TRANSIENT) == 0)
    {
      //setAutostartSetting(options);
    }

    // 5: Lazy?
    if ((options & Bundle::START_ACTIVATION_POLICY) != 0 && lazyActivation)
    {
      // 4: Resolve bundle (if needed)
      if (Bundle::STATE_INSTALLED == GetUpdatedState(this, l))
      {
        throw resolveFailException;
      }
      if (Bundle::STATE_STARTING == state)
      {
        return;
      }
      state = Bundle::STATE_STARTING;
      bundleContext.Store(std::make_shared<BundleContextPrivate>(this));
      operation = BundlePrivate::OP_ACTIVATING;
    }
    else
    {
      FinalizeActivation(l);
      return;
    }
  }
  // Last step of lazy activation
  coreCtx->listeners.BundleChanged(BundleEvent(BundleEvent::BUNDLE_LAZY_ACTIVATION, MakeBundle(this->shared_from_this())));
  {
    auto l = coreCtx->resolver.Lock(); US_UNUSED(l);
    operation = BundlePrivate::OP_IDLE;
    coreCtx->resolver.NotifyAll();
  }
}

std::exception_ptr BundlePrivate::Start0()
{
  bool bStarted = false;
  // res is used to signal that start did not complete in a normal way
  std::exception_ptr res;

  coreCtx->listeners.BundleChanged(BundleEvent(BundleEvent::BUNDLE_STARTING, MakeBundle(this->shared_from_this())));

  try
  {
    typedef BundleActivator*(*CreateActivatorHook)(void);
    CreateActivatorHook createActivatorHook = nullptr;

    void* libHandle = nullptr;
    if(IsSharedLibrary(lib.GetFilePath()))
    {
      if (!lib.IsLoaded())
      {
        lib.Load();
      }
      libHandle = lib.GetHandle();
    }
    else
    {
      libHandle = BundleUtils::GetExecutableHandle();
    }


    auto ctx = bundleContext.Load();

    // save this bundle's context so that it can be accessible anywhere
    // from within this bundle's code.
    std::string set_bundle_context_func = US_STR(US_SET_CTX_PREFIX) + symbolicName;
    void* setBundleContextSym = BundleUtils::GetSymbol(libHandle, set_bundle_context_func.c_str());
    std::memcpy(&SetBundleContext, &setBundleContextSym, sizeof(void*));
    if (SetBundleContext)
    {
      SetBundleContext(ctx.get());
    }

    // get the create/destroy activator callbacks
    std::string create_activator_func = US_STR(US_CREATE_ACTIVATOR_PREFIX) + symbolicName;
    void* createActivatorHookSym = BundleUtils::GetSymbol(libHandle, create_activator_func.c_str());
    std::memcpy(&createActivatorHook, &createActivatorHookSym, sizeof(void*));

    std::string destroy_activator_func = US_STR(US_DESTROY_ACTIVATOR_PREFIX) + symbolicName;
    void* destroyActivatorHookSym = BundleUtils::GetSymbol(libHandle, destroy_activator_func.c_str());
    std::memcpy(&destroyActivatorHook, &destroyActivatorHookSym, sizeof(void*));

    // try to get a BundleActivator instance
    if (createActivatorHook != nullptr && destroyActivatorHook != nullptr)
    {
      bactivator = std::unique_ptr<BundleActivator, DestroyActivatorHook>(createActivatorHook(), destroyActivatorHook);
      bactivator->Start(MakeBundleContext(ctx));
      bStarted = true;
    }

    if (!bStarted)
    {
      // Even bundles without an activator are marked as
      // ACTIVE.
      // Should we possible log an information message to
      // make sure users are aware of the missing activator?
    }

  }
  catch (const std::exception& e)
  {
    res = std::make_exception_ptr(std::runtime_error("Bundle#" + cppmicroservices::ToString(id) + " start failed: " + e.what()));
  }

  // activator.start() done
  // - normal -> state = active, started event
  // - exception from start() -> res = ex, start-failed clean-up
  // - unexpected state change (uninstall, refresh?):
  //   -> res = new exception
  // - start time-out -> res = new exception (not used?)

  // if start was aborted (uninstall or timeout), make sure
  // finalizeActivation() has finished before checking aborted/state
  {
    auto l = coreCtx->resolver.Lock(); US_UNUSED(l);
    if (!IsBundleThread(std::this_thread::get_id()))
    {
      // newer BundleThread instance has been active for this BundleImpl,
      // end thread execution
      throw std::runtime_error("Aborted bundle thread ending execution");
    }

    std::string cause;
    if (static_cast<Aborted>(aborted.load()) == Aborted::YES)
    {
      if (Bundle::STATE_UNINSTALLED == state)
      {
        cause = "Bundle uninstalled during Start()";
      }
      else
      {
        cause = "Bundle activator Start() time-out";
      }
    }
    else
    {
      aborted = static_cast<uint8_t>(Aborted::NO); // signal to other thread that BundleThread
                                                   // concludes start/stop
      if (Bundle::STATE_STARTING != state)
      {
        cause = "Bundle changed state because of refresh during Start()";
      }
    }
    if (!cause.empty())
    {
      res = std::make_exception_ptr("Bundle#" + cppmicroservices::ToString(id) + " start failed: " + cause);
    }
  }

  if (coreCtx->debug.lazyActivation)
  {
    DIAG_LOG(*coreCtx->sink) << "activating #" << id << " completed.";
  }

  if (res == nullptr)
  {
    // 10:
    state = Bundle::STATE_ACTIVE;
    coreCtx->listeners.BundleChanged(BundleEvent(BundleEvent::BUNDLE_STARTED, MakeBundle(this->shared_from_this())));
  }
  else if (operation == OP_ACTIVATING)
  {
    // 8:
    StartFailed();
  }
  return res;
}

void BundlePrivate::StartFailed()
{
  // 8:
  state = Bundle::STATE_STOPPING;
  coreCtx->listeners.BundleChanged(BundleEvent(BundleEvent::BUNDLE_STOPPING, MakeBundle(this->shared_from_this())));
  RemoveBundleResources();
  bundleContext.Exchange(std::shared_ptr<BundleContextPrivate>())->Invalidate();
  state = Bundle::STATE_RESOLVED;
  coreCtx->listeners.BundleChanged(BundleEvent(BundleEvent::BUNDLE_STOPPED, MakeBundle(this->shared_from_this())));
}

std::shared_ptr<BundleThread> BundlePrivate::GetBundleThread()
{
  auto l = coreCtx->bundleThreads.Lock(); US_UNUSED(l);

  // clean up old zombies
  for (auto bt : coreCtx->bundleThreads.zombies)
  {
    bt->Join();
  }
  coreCtx->bundleThreads.zombies.clear();

  if (coreCtx->bundleThreads.value.empty())
  {
    bundleThread = std::make_shared<BundleThread>(coreCtx);
  }
  else
  {
    bundleThread = coreCtx->bundleThreads.value.front();
    coreCtx->bundleThreads.value.pop_front();
  }

  return bundleThread;
}

bool BundlePrivate::IsBundleThread(const std::thread::id& id) const
{
  return bundleThread != nullptr && *bundleThread == id;
}

void BundlePrivate::ResetBundleThread()
{
  bundleThread = nullptr;
}

bool BundlePrivate::IsFragment() const
{
  return fragment != nullptr;
}

BundlePrivate::BundlePrivate(CoreBundleContext* coreCtx)
  : coreCtx(coreCtx)
  , id(0)
  , location(Constants::SYSTEM_BUNDLE_LOCATION)
  , state(Bundle::STATE_INSTALLED)
  , barchive()
  , bundleDir(this->coreCtx->GetDataStorage(id))
  , bundleContext()
  , destroyActivatorHook(nullptr)
  , bactivator(nullptr, nullptr)
  , operation(static_cast<uint8_t>(OP_IDLE))
  , resolveFailException()
  , wasStarted(false)
  , aborted(static_cast<uint8_t>(Aborted::NONE))
  , bundleThread()
  , symbolicName(Constants::SYSTEM_BUNDLE_SYMBOLICNAME)
  , version(CppMicroServices_MAJOR_VERSION, CppMicroServices_MINOR_VERSION, CppMicroServices_PATCH_VERSION)
  , fragment()
  , lazyActivation(false)
  , timeStamp(Clock::now())
  , fragments()
  , bundleManifest()
  , lib()
  , SetBundleContext(nullptr)
{
}

BundlePrivate::BundlePrivate(
    CoreBundleContext* coreCtx,
    const std::shared_ptr<BundleArchive>& ba
    )
  : coreCtx(coreCtx)
  , id(ba->GetBundleId())
  , location(ba->GetBundleLocation())
  , state(Bundle::STATE_INSTALLED)
  , barchive(ba)
  , bundleDir(coreCtx->GetDataStorage(id))
  , bundleContext()
  , destroyActivatorHook(nullptr)
  , bactivator(nullptr, nullptr)
  , operation(OP_IDLE)
  , resolveFailException()
  , wasStarted(false)
  , aborted(static_cast<uint8_t>(Aborted::NONE))
  , bundleThread()
  , symbolicName(ba->GetResourcePrefix())
  , version()
  , fragment()
  , lazyActivation(false)
  , timeStamp(ba->GetLastModified())
  , fragments()
  , bundleManifest()
  , lib(location)
  , SetBundleContext(nullptr)
{
  // Check if the bundle provides a manifest.json file and if yes, parse it.
  if (barchive->IsValid())
  {
    auto manifestRes = barchive->GetResource("/manifest.json");
    if (manifestRes)
    {
      BundleResourceStream manifestStream(manifestRes);
      try
      {
        bundleManifest.Parse(manifestStream);
      }
      catch (const std::exception& e)
      {
        throw std::runtime_error(std::string("Parsing of manifest.json for bundle ") + symbolicName + " at " + location + " failed: " + e.what());
      }
    }
  }

  // Check if we got version information and validate the version identifier
  if (bundleManifest.Contains(Constants::BUNDLE_VERSION))
  {
    Any versionAny = bundleManifest.GetValue(Constants::BUNDLE_VERSION);
    std::string errMsg;
    if (versionAny.Type() != typeid(std::string))
    {
      errMsg = std::string("The version identifier must be a string");
    }
    try
    {
      version = BundleVersion(versionAny.ToString());
    }
    catch (const std::exception& e)
    {
      errMsg = std::string("The version identifier is invalid: ") + e.what();
    }

    if (!errMsg.empty())
    {
      throw std::invalid_argument(std::string("The Json value for ") + Constants::BUNDLE_VERSION + " for bundle " +
                                  symbolicName + " is not valid: " + errMsg);
    }
  }

  if (!bundleManifest.Contains(Constants::BUNDLE_SYMBOLICNAME))
  {
    throw std::invalid_argument(Constants::BUNDLE_SYMBOLICNAME + " is not defined in the bundle manifest for bundle " + symbolicName + ".");
  }

  Any bsn(bundleManifest.GetValue(Constants::BUNDLE_SYMBOLICNAME));
  if (bsn.Empty())
  {
    throw std::invalid_argument(Constants::BUNDLE_SYMBOLICNAME + " is empty in the bundle manifest for bundle " + symbolicName + ".");
  }

  auto snbl = coreCtx->bundleRegistry.GetBundles(symbolicName, version);
  if (!snbl.empty())
  {
    throw std::invalid_argument("Bundle#" + cppmicroservices::ToString(id) +
                                ", a bundle with same symbolic name and version " +
                                "is already installed (" + symbolicName + ", " +
                                version.ToString() + ")");
  }

  // $TODO extensions
  // Activate extension as soon as they are installed so that
  // they get added in bundle id order.
  /*
  if (gen.IsExtension() && AttachToFragmentHost(coreCtx->systemBundle->Current()))
  {
    gen.SetWired();
    state = Bundle::STATE_RESOLVED;
  }
  */
}

BundlePrivate::~BundlePrivate()
{
}

void BundlePrivate::CheckUninstalled() const
{
  if (state == Bundle::STATE_UNINSTALLED)
  {
    throw std::logic_error("Bundle is in UNINSTALLED state");
  }
}

void BundlePrivate::RemoveBundleResources()
{
  coreCtx->listeners.RemoveAllListeners(bundleContext.Load());

  std::vector<ServiceRegistrationBase> srs;
  coreCtx->services.GetRegisteredByBundle(this, srs);
  for (std::vector<ServiceRegistrationBase>::iterator i = srs.begin();
       i != srs.end(); ++i)
  {
    try
    {
      i->Unregister();
    }
    catch (const std::logic_error& /*ignore*/)
    {
      // Someone has unregistered the service after stop completed.
      // This should not occur, but we don't want get stuck in
      // an illegal state so we catch it.
    }
  }

  srs.clear();
  coreCtx->services.GetUsedByBundle(this, srs);
  for (std::vector<ServiceRegistrationBase>::const_iterator i = srs.begin();
       i != srs.end(); ++i)
  {
    i->GetReference(std::string()).d.load()->UngetService(this->shared_from_this(), false);
  }
}

void BundlePrivate::Purge()
{
  //coreCtx->bundles.RemoveZombie(this);
  if (barchive->IsValid())
  {
    barchive->Purge();
  }
  //ClearWiring();
}

std::shared_ptr<BundleArchive> BundlePrivate::GetBundleArchive() const
{
  return barchive;
}

void BundlePrivate::SetAutostartSetting(int32_t setting)
{
  if (barchive->IsValid())
  {
    barchive->SetAutostartSetting(setting);
  }
}

int32_t BundlePrivate::GetAutostartSetting() const
{
  return barchive->IsValid() ? barchive->GetAutostartSetting() : -1;
}

std::shared_ptr<BundlePrivate> GetPrivate(const Bundle& b)
{
  return b.d;
}

}