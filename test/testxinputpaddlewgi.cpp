/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty. In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

// Offline tests compile the production state, aggregation, and command runner.
// This compile-time boundary removes all production startup and WGI activation.
#define SDL_XINPUT_PADDLE_WGI_OFFLINE_TEST
#include "../src/joystick/windows/SDL_xinput_paddle_wgi.cpp"
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sdl_paddles;
static std::atomic<unsigned> checks{0};
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error(std::string("line ")+std::to_string(__LINE__)+": " #x); } while (false)

struct Counts { unsigned providers = 0, outers = 0; bool releaseLocked = false; };

class FakeProvider final : public custom::IGipGameControllerProvider, public custom::IGameControllerProvider {
    std::atomic<ULONG> refs{1};
    State& state;
    Counts& counts;
public:
    const wchar_t* publicId=L"GIP:000000000000000B";
    USHORT vendor=0x045e, product=0x0b00;
    HRESULT publicError=S_OK;
    bool hasGip=true;
    unsigned unsupportedQueries=0;
    FakeProvider(State& s, Counts& c) : state(s), counts(c) {}
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        if (TryAcquireSRWLockExclusive(&state.lock)) ReleaseSRWLockExclusive(&state.lock);
        else counts.releaseLocked = true;
        ULONG remaining = --refs;
        if (!remaining) { ++counts.providers; delete this; }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) noexcept override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IInspectable) || id == __uuidof(custom::IGameControllerProvider)) *out=static_cast<custom::IGameControllerProvider*>(this);
        else if (id==__uuidof(custom::IGipGameControllerProvider)&&hasGip) *out=static_cast<custom::IGipGameControllerProvider*>(this);
        else { ++unsupportedQueries; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** ids) noexcept override { if (!count || !ids) return E_POINTER; *count=0; *ids=nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* value) noexcept override { if (!value) return E_POINTER; *value=nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* value) noexcept override { if (!value) return E_POINTER; *value=BaseTrust; return S_OK; }
    HRESULT STDMETHODCALLTYPE SendMessage(custom::GipMessageClass,BYTE,UINT32,BYTE*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendReceiveMessage(custom::GipMessageClass,BYTE,UINT32,BYTE*,UINT32,BYTE*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UpdateFirmwareAsync(ABI::Windows::Storage::Streams::IInputStream*,ABI::Windows::Foundation::IAsyncOperationWithProgress<custom::GipFirmwareUpdateResult*,custom::GipFirmwareUpdateProgress>**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_FirmwareVersionInfo(custom::GameControllerVersionInfo*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_HardwareVersionInfo(custom::GameControllerVersionInfo*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_HardwareVendorId(USHORT* value) noexcept override { *value=vendor;return S_OK; }
    HRESULT STDMETHODCALLTYPE get_HardwareProductId(USHORT* value) noexcept override { *value=product;return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsConnected(boolean* value) noexcept override { *value=true;return S_OK; }
};

namespace sdl_paddles { namespace {
HRESULT PublicProviderId(custom::IGameControllerProvider* provider,HSTRING* value) noexcept {
    auto* fake=static_cast<FakeProvider*>(provider);
    if(FAILED(fake->publicError))return fake->publicError;
    return WindowsCreateString(fake->publicId,static_cast<UINT32>(std::wcslen(fake->publicId)),value);
}
} }

class FakeOuter final : public input::IGameController {
    std::atomic<ULONG> refs{1};
    Inner* inner;
    Counts& counts;
public:
    FakeOuter(Inner* value, Counts& c) : inner(value),counts(c) {}
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG remaining=--refs;
        if (!remaining) { inner->Release(); ++counts.outers; delete this; }
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) noexcept override {
        if (!value) return E_POINTER;
        *value=nullptr;
        if (id==__uuidof(IUnknown)||id==__uuidof(IInspectable)||id==__uuidof(input::IGameController)) { *value=this; AddRef(); return S_OK; }
        return inner->QueryInterface(id,value);
    }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count,IID** ids) noexcept override { if(!count||!ids)return E_POINTER;*count=0;*ids=nullptr;return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* value) noexcept override { if(!value)return E_POINTER;*value=nullptr;return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* value) noexcept override { if(!value)return E_POINTER;*value=BaseTrust;return S_OK; }
    using HeadsetHandler=__FITypedEventHandler_2_Windows__CGaming__CInput__CIGameController_Windows__CGaming__CInput__CHeadset;
    using UserHandler=__FITypedEventHandler_2_Windows__CGaming__CInput__CIGameController_Windows__CSystem__CUserChangedEventArgs;
    HRESULT STDMETHODCALLTYPE add_HeadsetConnected(HeadsetHandler*,EventRegistrationToken*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE remove_HeadsetConnected(EventRegistrationToken) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE add_HeadsetDisconnected(HeadsetHandler*,EventRegistrationToken*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE remove_HeadsetDisconnected(EventRegistrationToken) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE add_UserChanged(UserHandler*,EventRegistrationToken*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE remove_UserChanged(EventRegistrationToken) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_Headset(input::IHeadset** value) noexcept override { if(!value)return E_POINTER;*value=nullptr;return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsWireless(boolean* value) noexcept override { if(!value)return E_POINTER;*value=false;return S_OK; }
    HRESULT STDMETHODCALLTYPE get_User(ABI::Windows::System::IUser** value) noexcept override { if(!value)return E_POINTER;*value=nullptr;return S_OK; }
};

void Resume(Inner* inner, UINT64 timestamp) {
    ComPtr<custom::IGameControllerInputSink> sink;
    CHECK(inner->QueryInterface(IID_PPV_ARGS(&sink))==S_OK);
    CHECK(sink->OnInputResumed(timestamp)==S_OK);
}

void Suspend(Inner* inner, UINT64 timestamp) {
    ComPtr<custom::IGameControllerInputSink> sink;
    CHECK(inner->QueryInterface(IID_PPV_ARGS(&sink))==S_OK);
    CHECK(sink->OnInputSuspended(timestamp)==S_OK);
}

HRESULT Message(Inner* inner, UINT64 timestamp, custom::GipMessageClass messageClass,
                BYTE id, BYTE sequence, UINT32 size, const BYTE* bytes) {
    ComPtr<custom::IGipGameControllerInputSink> sink;
    CHECK(inner->QueryInterface(IID_PPV_ARGS(&sink))==S_OK);
    return sink->OnMessageReceived(timestamp,messageClass,id,sequence,size,const_cast<BYTE*>(bytes));
}

void Normal(Inner* inner, UINT64 timestamp) {
    const std::array<BYTE,46> bytes{};
    CHECK(Message(inner,timestamp,custom::GipMessageClass_LowLatency,0,1,
                  static_cast<UINT32>(bytes.size()),bytes.data())==S_OK);
}

struct Attachment {
    State& state;
    Ticket ticket;
    Inner* inner;
    FakeOuter* outer;
    Attachment(State& s, Counts& counts, std::uint64_t id, bool ready=true) : state(s) {
        ticket=state.Reserve(); CHECK(ticket.serial);
        auto* provider=new FakeProvider(state,counts);
        inner=new Inner(state,ticket);
        CHECK(state.Install(ticket,id,provider));
        outer=new FakeOuter(inner,counts);
        CHECK(inner->Initialize(outer,nullptr)==S_OK);
        state.Added(outer);
        if (ready) { Resume(inner,1); Normal(inner,2); }
    }
    void Remove() { if (outer) { state.Removed(outer); outer->Release(); outer=nullptr; } }
    ~Attachment() { Remove(); while(state.CleanupOne()) {} }
};

GipInputState Input(State& state, std::uint64_t id) {
    GipInputState result;
    CHECK(state.InputState(id,result));
    return result;
}

std::uint64_t Submit(State& state, std::uint64_t id, std::uint64_t generation,
                     std::uint64_t now, bool exhausted=false) {
    const auto inputState=Input(state,id);
    return state.Submit(id,generation,inputState.provider,inputState.epoch,now,exhausted);
}

void Succeed(State& state, const Job& job, std::uint64_t now, unsigned& sends) {
    CHECK(job.token);
    Execute(state,job,[](auto*){return S_OK;},[&](auto*,auto& request,auto& response){
        CHECK(TryAcquireSRWLockExclusive(&state.lock)); ReleaseSRWLockExclusive(&state.lock);
        CHECK(request[0]==7&&request[1]==0&&response.size()==64&&response[0]==0xff);
        response[0]=7; response[1]=0; ++sends;
        return S_OK;
    },[=]{return now;});
}

template<std::size_t Size> std::array<BYTE,Size> Hex(const char* text) {
    CHECK(std::strlen(text)==Size*2);
    auto digit=[](char c)->BYTE {
        CHECK((c>='0'&&c<='9')||(c>='a'&&c<='f'));
        return static_cast<BYTE>(c<='9' ? c-'0' : c-'a'+10);
    };
    std::array<BYTE,Size> bytes{};
    for(std::size_t i=0;i<Size;++i) bytes[i]=static_cast<BYTE>((digit(text[i*2])<<4)|digit(text[i*2+1]));
    return bytes;
}

GipEnableResult Result(State& s,std::uint64_t token,std::uint64_t now) {
    GipEnableResult result;
    CHECK(s.Poll(token,result,now));
    return result;
}

void Identifiers() {
    std::uint64_t id;
    CHECK(ParseProviderId(L"GIP:0000A806FF8EED7E",20,id)&&id==0x0000a806ff8eed7e);
    CHECK(ParseProviderId(L"GIP:0123456789abcdef",20,id)&&id==0x0123456789abcdef);
    CHECK(!ParseProviderId(L"GIP:0000000000000000",20,id));
    CHECK(!ParseProviderId(L"GIP:0000A806FF8EED7Z",20,id));
    CHECK(!ParseProviderId(L"XIP:0000A806FF8EED7E",20,id));
    CHECK(!ParseProviderId(nullptr,20,id));
    CHECK(!ParseProviderId(L"GIP:0",5,id));
    CHECK(!ParseProviderId(L"GIP:0000A806FF8EED7E0",21,id));
    CHECK(ParseProviderId(L"GIP:0000000000000001",20,id)&&id==1);
    CHECK(ParseProviderId(L"GIP:FFFFFFFFFFFFFFFF",20,id)&&id==UINT64_MAX);
    CHECK(ParseProviderId(L"GIP:ffffffffffffffff",20,id)&&id==UINT64_MAX);
    CHECK(!ParseProviderId(L"gip:0000A806FF8EED7E",20,id));
    CHECK(!ParseProviderId(L"GIP: 000A806FF8EED7E",20,id));
    CHECK(!ParseProviderId(L"GIP:0000A806FF8EED7 ",20,id));
    CHECK(!ParseProviderId(L"GIP:+000A806FF8EED7E",20,id));
    CHECK(!ParseProviderId(L"GIP:0000A806FF8EED7E",19,id));
    wchar_t embedded[]=L"GIP:0000A806FF8EED7E";
    embedded[8]=0;
    CHECK(!ParseProviderId(embedded,20,id)&&id==0);
    embedded[8]=L'\uff10';
    CHECK(!ParseProviderId(embedded,20,id)&&id==0);
    CHECK(Eligible(0x045e,0x02e3)&&Eligible(0x045e,0x0b00)&&Eligible(0x045e,0x0b05)&&Eligible(0x045e,0x0b22));
    CHECK(!Eligible(0x045e,0x0b13)&&!Eligible(0x1234,0x0b00));
}

void CommandsAndTimeout() {
    State s; Counts counts; Attachment a(s,counts,11);
    auto t=Submit(s,11,1,100); CHECK(t);
    CHECK(Submit(s,11,1,101)==t);
    auto job=s.Claim(101); CHECK(job.token==t);
    unsigned sends=0;
    Succeed(s,job,102,sends);
    auto result=Result(s,t,103);
    CHECK(sends==1&&result.status==Status::Success&&result.dispatched);
    CHECK(result.provider==a.ticket.serial&&result.epoch==Input(s,11).epoch);
    CHECK(!Input(s,11).splitSeen);
    t=Submit(s,11,2,200); job=s.Claim(201); CHECK(job.token==t);
    CHECK(s.Authorize(job,202));
    CHECK(!s.Authorize(job,203));
    CHECK(Result(s,t,10200).status==Status::TimedOut);
    CHECK(s.requests[job.request].working&&s.catalog[a.ticket.index].borrowers==1);
    CHECK(Submit(s,11,2,10200)==t);
    s.Complete(job,S_OK,10201);
    CHECK(Result(s,t,10202).status==Status::TimedOut&&Result(s,t,10202).dispatched);
    CHECK(!s.requests[job.request].working&&!Input(s,11).busy);
    CHECK(Submit(s,11,2,10203)==t&&!s.Claim(10203).token);
    t=Submit(s,11,3,11000); job=s.Claim(11001);
    Execute(s,job,[&](auto*){s.Retire(3);return S_OK;},[&](auto*,auto&,auto&){++sends;return S_OK;},[]{return 11002ULL;});
    CHECK(sends==1&&Result(s,t,11003).status==Status::Retired&&!Result(s,t,11003).dispatched);
    t=Submit(s,11,4,12000); job=s.Claim(12001);
    Execute(s,job,[](auto*){return E_ACCESSDENIED;},[&](auto*,auto&,auto&){++sends;return S_OK;},[]{return 12002ULL;});
    CHECK(Result(s,t,12003).hresult==E_ACCESSDENIED&&!Result(s,t,12003).dispatched&&sends==1);
    CHECK(Submit(s,11,4,12004)==t&&!s.Claim(12004).token);
    t=Submit(s,11,5,13000); job=s.Claim(13001);
    Execute(s,job,[](auto*){return S_OK;},[&](auto*,auto&,auto&){s.Retire(5);return S_OK;},[]{return 13002ULL;});
    CHECK(Result(s,t,13003).status==Status::Retired&&Result(s,t,13003).dispatched);
}

void QueuesAndExhaustion() {
    State s; Counts counts;
    std::vector<std::unique_ptr<Attachment>> attachments;
    std::array<Job,4> held;
    for(unsigned i=0;i<4;++i) {
        attachments.push_back(std::make_unique<Attachment>(s,counts,11+i));
        CHECK(Submit(s,11+i,i+1,100));
        held[i]=s.Claim(101); CHECK(held[i].token);
    }
    Attachment queued(s,counts,55);
    for(unsigned i=0;i<16;++i) CHECK(Submit(s,55,i+10,100));
    CHECK(!Submit(s,55,100,100));
    for(auto& j:held) CHECK(Result(s,j.token,10100).status==Status::TimedOut);
    for(unsigned i=0;i<16;++i) CHECK(Submit(s,55,i+100,10200,true));
    for(auto& j:held) {
        CHECK(s.requests[j.request].token==j.token);
        s.Complete(j,S_OK,10300);
    }
    auto t=Submit(s,55,1000,10400,true);
    CHECK(t&&Result(s,t,10400).status==Status::Exhausted);
    s.nextToken=UINT64_MAX; CHECK(!Submit(s,55,1001,10400));
    Broker b;
    CHECK(b.Exhausted(10000));
    b.workers[0].alive=true; b.workers[0].busy=true; b.workers[0].since=100;
    CHECK(!b.Exhausted(10099)&&b.Exhausted(10100));
    b.workers[1].alive=true; b.workers[1].busy=false;
    CHECK(!b.Exhausted(50000));
}

void LateCatalogAndCrossRemoval() {
    State s; Counts counts;
    CHECK(!Input(s,11).provider&&!Submit(s,11,1,0));
    CHECK(!s.Claim(1).token);
    Attachment a(s,counts,11), b(s,counts,22);
    auto first=Submit(s,11,1,1),second=Submit(s,22,2,1);
    CHECK(first&&second);
    auto job=s.Claim(2); CHECK(job.token==first);
    auto factory=Microsoft::WRL::Make<Factory>(s);
    CHECK(factory->OnGameControllerRemoved(a.outer)==S_OK);
    CHECK(Result(s,first,3).status==Status::Retired);
    CHECK(Input(s,22).Ready());
    a.Remove();
    CHECK(!s.CleanupOne());
    Attachment reused(s,counts,11);
    auto replacement=Submit(s,11,3,3); CHECK(replacement);
    s.Complete(job,S_OK,4);
    CHECK(Result(s,first,4).status==Status::Retired);
    CHECK(Result(s,replacement,4).status==Status::Pending);
    CHECK(s.CleanupOne());
    unsigned sends=0;
    auto next=s.Claim(5); CHECK(next.token==second); Succeed(s,next,6,sends);
    next=s.Claim(7); CHECK(next.token==replacement&&next.catalog.serial==reused.ticket.serial);
    Succeed(s,next,8,sends);
    CHECK(sends==2&&!counts.releaseLocked);
}

void OldCleanupAndNewPending() {
    State s; Counts counts; Attachment old(s,counts,11);
    const auto oldInput=Input(s,11);
    auto token=Submit(s,11,1,0); auto job=s.Claim(1); CHECK(job.token==token);
    old.Remove();
    CHECK(!Input(s,11).provider);
    CHECK(!s.Submit(11,2,oldInput.provider,oldInput.epoch,2,false));
    s.Complete(job,S_OK,3);
    CHECK(s.CleanupOne());
    Attachment replacement(s,counts,11);
    CHECK(Input(s,11).provider!=oldInput.provider);
    CHECK(!s.Submit(11,2,oldInput.provider,oldInput.epoch,3,false));
    auto later=Submit(s,11,2,3); CHECK(later);
    job=s.Claim(4); CHECK(job.token==later);
    unsigned sends=0; Succeed(s,job,5,sends);
    CHECK(Result(s,later,6).status==Status::Success);
    auto next=Submit(s,11,3,7); job=s.Claim(8);
    Attachment ambiguous(s,counts,11);
    s.Complete(job,S_OK,9);
    CHECK(Result(s,next,10).status==Status::Retired);
    const auto duplicate=Input(s,11);
    CHECK(!duplicate.Ready()&&duplicate.error==HRESULT_FROM_WIN32(ERROR_DUP_NAME));
    CHECK(!Submit(s,11,4,10));
}

void AggregationAndLimits() {
    State s; Counts counts;
    {
        Attachment a(s,counts,11,false);
        ComPtr<custom::IGipGameControllerInputSink> sink;
        CHECK(a.inner->QueryInterface(IID_PPV_ARGS(&sink))==S_OK);
        ComPtr<IUnknown> identity;
        CHECK(sink.As(&identity)==S_OK&&identity.Get()==static_cast<IUnknown*>(a.outer));
        CHECK(sink->OnMessageReceived(1,custom::GipMessageClass_StandardLatency,12,0,UINT32_MAX,nullptr)==E_POINTER);
        CHECK(!Input(s,11).Ready());
        CHECK(a.inner->Initialize(a.outer,nullptr)==E_UNEXPECTED);
        a.Remove();
        CHECK(s.CleanupOne());
        CHECK(s.Cell(a.ticket)); // The retained sink still owns the aggregate.
        identity.Reset(); sink.Reset();
        CHECK(s.CleanupOne()&&!s.Cell(a.ticket));
    }
    CHECK(counts.providers==1&&counts.outers==1&&!counts.releaseLocked);
    std::vector<Ticket> cells;
    for(unsigned i=0;i<16;++i) { auto t=s.Reserve(); CHECK(t.serial); cells.push_back(t); }
    CHECK(!s.Reserve().serial);
    for(auto t:cells) s.CancelReserve(t);
    auto t=s.Reserve(); CHECK(t.serial); s.CancelReserve(t);
    s.nextSerial=UINT64_MAX; CHECK(!s.Reserve().serial);
    State duplicate; Counts other;
    Attachment a(duplicate,other,42),b(duplicate,other,42);
    CHECK(Input(duplicate,42).error==HRESULT_FROM_WIN32(ERROR_DUP_NAME));
    CHECK(!Submit(duplicate,42,1,0));
}

void FactoryMetadata() {
    State s; Counts counts;
    auto factory=Microsoft::WRL::Make<Factory>(s);
    auto* provider=new FakeProvider(s,counts);
    IInspectable* value=nullptr;
    provider->publicError=E_ACCESSDENIED;
    CHECK(factory->CreateGameController(provider,&value)==E_ACCESSDENIED&&!value);
    provider->publicError=S_OK; provider->publicId=L"GIP:0000000000000000";
    CHECK(factory->CreateGameController(provider,&value)==E_INVALIDARG&&!value);
    provider->publicId=L"GIP:000000000000000B"; provider->hasGip=false;
    CHECK(factory->CreateGameController(provider,&value)==E_NOINTERFACE&&!value);
    provider->hasGip=true; provider->product=0x0b13;
    CHECK(factory->CreateGameController(provider,&value)==E_NOINTERFACE&&!value);
    provider->product=0x0b00; provider->unsupportedQueries=0;
    CHECK(factory->CreateGameController(provider,&value)==S_OK&&value);
    CHECK(provider->unsupportedQueries==0); // Public provider interfaces are sufficient.
    auto* inner=dynamic_cast<Inner*>(value); CHECK(inner);
    ComPtr<custom::IGipGameControllerInputSink> sink;
    CHECK(inner->QueryInterface(IID_PPV_ARGS(&sink))==E_UNEXPECTED&&!sink);
    CHECK(inner->Initialize(nullptr,nullptr)==E_POINTER);
    auto* outer=new FakeOuter(inner,counts);
    CHECK(inner->Initialize(outer,provider)==S_OK);
    CHECK(factory->OnGameControllerAdded(outer)==S_OK);
    CHECK(!Input(s,11).Ready());
    Resume(inner,1); Normal(inner,2);
    CHECK(!Submit(s,12,2,0));
    CHECK(!s.Claim(0).token);
    auto token=Submit(s,11,1,0); auto job=s.Claim(1); CHECK(job.token==token);
    unsigned sends=0; Succeed(s,job,2,sends);
    CHECK(factory->OnGameControllerRemoved(outer)==S_OK);
    CHECK(!Input(s,11).provider);
    outer->Release(); CHECK(s.CleanupOne());
    provider->Release();
    CHECK(counts.providers==1&&counts.outers==1&&!counts.releaseLocked);
    for(const auto& cell:s.catalog) CHECK(!cell.occupied);
    s.Added(nullptr); s.Removed(nullptr); CHECK(!s.BindOuter({},nullptr));
}

struct InstalledBroker {
    Broker value;
    Broker* previous;
    InstalledBroker() : previous(broker.exchange(&value)) { value.workers[0].alive=true; }
    ~InstalledBroker() { broker.store(previous); }
};

void ApiAndContention() {
    {
        InstalledBroker installed;
        auto& s=installed.value.state;
        Counts counts;
        GipInputState inputState;
        CHECK(QueryGipInputState(0,inputState)&&inputState.error==E_INVALIDARG&&!inputState.Ready());
        CHECK(QueryGipInputState(123,inputState)&&!inputState.provider&&!inputState.Ready());
        Attachment a(s,counts,123,false);
        CHECK(QueryGipInputState(123,inputState)&&inputState.provider&&!inputState.Ready());
        CHECK(!SubmitGipEnable(123,4,inputState.provider,inputState.epoch));
        Resume(a.inner,1); Normal(a.inner,2);
        CHECK(QueryGipInputState(123,inputState)&&inputState.Ready());
        CHECK(!SubmitGipEnable(0,1,1,1)&&!SubmitGipEnable(1,0,1,1));
        CHECK(!SubmitGipEnable(123,4,0,inputState.epoch)&&!SubmitGipEnable(123,4,inputState.provider,0));
        auto t=SubmitGipEnable(123,4,inputState.provider,inputState.epoch); CHECK(t);
        GipEnableResult result;
        CHECK(PollGipEnable(t,result)&&result.status==Status::Pending&&!result.dispatched);
        CHECK(result.provider==inputState.provider&&result.epoch==inputState.epoch);
        AcquireSRWLockExclusive(&s.lock);
        const bool submitBlocked=!SubmitGipEnable(123,5,inputState.provider,inputState.epoch);
        const bool pollBlocked=!PollGipEnable(t,result);
        const bool queryBlocked=!QueryGipInputState(123,inputState);
        ReleaseSRWLockExclusive(&s.lock);
        CHECK(submitBlocked&&pollBlocked&&queryBlocked);
        RetireGipEnable(4);
        CHECK(PollGipEnable(t,result)&&result.status==Status::Retired);
        CHECK(!PollGipEnable(UINT64_MAX,result));
    }
    GipInputState inputState;
    CHECK(!QueryGipInputState(123,inputState));
    CHECK(!SubmitGipEnable(123,1,1,1)); // Offline startup cannot activate WGI.
}

void CapturedReadinessAndRearm() {
    State s; Counts counts; Attachment a(s,counts,11,false);
    unsigned sends=0;
    // wgi-usb-combined-66644-158953984.log:24-26 sent before any own input.
    auto inputState=Input(s,11);
    CHECK(!inputState.Ready()&&!inputState.epoch);
    CHECK(!s.Submit(11,7,inputState.provider,inputState.epoch,158954156,false));
    CHECK(!s.Claim(158954171).token&&sends==0);
    // wgi-usb-active-47744-159649937.log:40,44,47,55. These are SDK
    // callback timestamps. The command runner's host tick has a separate clock.
    constexpr UINT64 resumed=159794300523ULL,normal=159797350861ULL;
    CHECK(normal>resumed);
    Resume(a.inner,resumed);
    inputState=Input(s,11);
    CHECK(inputState.resumed&&!inputState.normalSeen&&!inputState.Ready()&&inputState.epoch==1);
    CHECK(!Submit(s,11,7,159794032));
    const auto bytes=Hex<46>("0000000000008dfdacfe31ff51fa000000003000000000000000000000000000000000000000e5e6212a5aea212a");
    CHECK(Message(a.inner,normal,custom::GipMessageClass_LowLatency,0,63,46,bytes.data())==S_OK);
    inputState=Input(s,11);
    CHECK(inputState.Ready()&&inputState.resumeTime==resumed&&inputState.normalTime==normal);
    auto token=Submit(s,11,7,159794046); CHECK(token);
    auto job=s.Claim(159794047); CHECK(job.token==token);
    Succeed(s,job,159794062,sends);
    const auto result=Result(s,token,159794063);
    CHECK(result.status==Status::Success&&result.dispatched&&sends==1);
    CHECK(!Input(s,11).splitSeen);
    // A successful method call and later normal input do not establish a split
    // stream and do not schedule another command in the same input epoch.
    for(unsigned i=0;i<3;++i) {
        Normal(a.inner,normal+100+i);
        CHECK(Submit(s,11,7,159894062+i)==token);
        CHECK(!s.Claim(159894062+i).token&&!Input(s,11).splitSeen);
    }
    Resume(a.inner,normal+1000);
    const auto second=Input(s,11);
    CHECK(second.epoch==inputState.epoch+1&&!second.Ready());
    CHECK(Result(s,token,159894066).status==Status::Retired);
    CHECK(s.Submit(11,7,inputState.provider,inputState.epoch,159894067,false)==token);
    CHECK(!s.Claim(159894067).token);
    Normal(a.inner,normal);
    CHECK(Input(s,11).Ready()&&Input(s,11).normalTime==normal);
    auto next=Submit(s,11,7,159894068); CHECK(next&&next!=token);
    job=s.Claim(159894069); Succeed(s,job,159894070,sends);
    CHECK(sends==2&&Result(s,next,159894071).epoch==second.epoch);
}

void NativeImplicitResumeOrder() {
    State s; Counts counts; Attachment a(s,counts,11,false);
    // GipDevice::ProcessInput samples its time at RVA 0x32EB7, then can
    // invoke ProcessFocusMessage at 0x330AE before OnMessageReceived.
    // That resume samples a later time but is delivered before this normal.
    Resume(a.inner,101);
    Normal(a.inner,100);
    const auto inputState=Input(s,11);
    CHECK(inputState.Ready()&&inputState.epoch==1);
    CHECK(inputState.resumeTime==101&&inputState.normalTime==100);
    auto token=Submit(s,11,7,1000); CHECK(token);
    auto job=s.Claim(1001); CHECK(job.token==token);
    unsigned sends=0; Succeed(s,job,1002,sends);
    CHECK(sends==1&&Result(s,token,1003).status==Status::Success);
}

void ColdStartAndShapes() {
    State s; Counts counts; Attachment cold(s,counts,11,false),peer(s,counts,22);
    std::array<BYTE,47> bytes{};
    CHECK(Input(s,22).Ready()&&!Input(s,11).Ready());
    CHECK(Message(cold.inner,10,custom::GipMessageClass_Command,0,1,46,bytes.data())==S_OK);
    CHECK(Message(cold.inner,11,custom::GipMessageClass_LowLatency,0x20,1,46,bytes.data())==S_OK);
    CHECK(Message(cold.inner,12,custom::GipMessageClass_LowLatency,0,1,45,bytes.data())==S_OK);
    CHECK(Message(cold.inner,13,custom::GipMessageClass_LowLatency,0,1,47,bytes.data())==S_OK);
    CHECK(Message(cold.inner,14,custom::GipMessageClass_LowLatency,0,1,46,nullptr)==E_POINTER);
    CHECK(Message(cold.inner,15,custom::GipMessageClass_Command,12,1,17,bytes.data())==S_OK);
    ComPtr<custom::IGipGameControllerInputSink> sink;
    CHECK(cold.inner->QueryInterface(IID_PPV_ARGS(&sink))==S_OK);
    CHECK(sink->OnKeyReceived(16,1,true)==S_OK);
    CHECK(!Input(s,11).Ready()&&!Input(s,11).epoch);
    Normal(cold.inner,100);
    auto inputState=Input(s,11);
    CHECK(inputState.Ready()&&inputState.epoch==1&&inputState.normalTime==100);
    Normal(cold.inner,101);
    CHECK(Input(s,11).epoch==1);
    Suspend(cold.inner,200);
    Normal(cold.inner,201);
    CHECK(!Input(s,11).Ready()&&!Input(s,11).normalSeen);
    Resume(cold.inner,300);
    CHECK(Input(s,11).epoch==2&&!Input(s,11).Ready());
    Normal(cold.inner,299);
    CHECK(Input(s,11).Ready()&&Input(s,11).normalTime==299);
    Normal(cold.inner,301);
    CHECK(Input(s,11).Ready());
    Resume(cold.inner,400);
    CHECK(Input(s,11).epoch==3&&!Input(s,11).Ready());
    Resume(cold.inner,400);
    CHECK(Input(s,11).epoch==3); // Duplicate notification with the same timestamp.
    Normal(cold.inner,401);
    Suspend(cold.inner,350);
    CHECK(Input(s,11).Ready()&&Input(s,11).epoch==3);
    Attachment suspended(s,counts,33,false);
    Suspend(suspended.inner,500);
    Normal(suspended.inner,501);
    CHECK(!Input(s,33).Ready()&&!Input(s,33).epoch);
    Resume(suspended.inner,502); Normal(suspended.inner,503);
    CHECK(Input(s,33).Ready());
}

void SuspendBeforeDispatch() {
    State s; Counts counts; Attachment a(s,counts,11);
    auto t=Submit(s,11,1,100); CHECK(t);
    const auto first=Input(s,11);
    Suspend(a.inner,3);
    CHECK(!s.Claim(101).token);
    CHECK(Result(s,t,102).status==Status::Retired&&!Result(s,t,102).dispatched);
    Resume(a.inner,4); Normal(a.inner,5);
    t=Submit(s,11,1,103); CHECK(t);
    auto job=s.Claim(104); CHECK(job.token==t);
    unsigned sends=0;
    Execute(s,job,[&](auto*){Suspend(a.inner,6);return S_OK;},
            [&](auto*,auto&,auto&){++sends;return S_OK;},[]{return 105ULL;});
    CHECK(!sends&&Result(s,t,106).status==Status::Retired&&!Result(s,t,106).dispatched);
    CHECK(!Input(s,11).busy);
    Resume(a.inner,7); Normal(a.inner,8);
    t=Submit(s,11,1,107); job=s.Claim(108); CHECK(job.token==t);
    Execute(s,job,[](auto*){return S_OK;},
            [&](auto*,auto&,auto&){++sends;Suspend(a.inner,9);return S_OK;},[]{return 109ULL;});
    auto result=Result(s,t,110);
    CHECK(sends==1&&result.status==Status::Retired&&result.dispatched);
    CHECK(result.provider==first.provider&&result.epoch==first.epoch+2);
    CHECK(!Input(s,11).Ready()&&!Input(s,11).busy);
}

template<class Predicate> void Await(Predicate predicate) {
    const auto deadline=GetTickCount64()+2000;
    while(!predicate()&&GetTickCount64()<deadline) std::this_thread::yield();
    CHECK(predicate());
}

void ConcurrentRetirement(bool remove=false) {
    State s; Counts counts; Attachment a(s,counts,11),b(s,counts,22);
    auto token=Submit(s,11,1,0); auto job=s.Claim(1); CHECK(job.token==token);
    auto peer=Submit(s,22,2,1000); CHECK(peer);
    std::atomic<bool> sending=false,finish=false;
    std::atomic<bool> providerValid=false;
    std::atomic<std::uint64_t> clock=1001;
    std::exception_ptr failure;
    std::jthread worker([&](std::stop_token stop) {
        try {
            Execute(s,job,[](auto*){return S_OK;},[&](auto* provider,auto& request,auto& response) {
                CHECK(request[0]==7&&response[0]==0xff);
                sending.store(true);
                while(!finish.load()&&!stop.stop_requested()) std::this_thread::yield();
                TrustLevel level=BaseTrust;
                providerValid.store(provider->GetTrustLevel(&level)==S_OK&&level==BaseTrust);
                response[0]=7; response[1]=0;
                return S_OK;
            },[&]{return clock.load();});
        } catch(...) { failure=std::current_exception(); }
    });
    Await([&]{return sending.load();});
    if(remove) {
        a.Remove();
        CHECK(!Input(s,11).provider&&!Input(s,11).Ready());
        CHECK(!Submit(s,11,3,1002)&&!s.CleanupOne());
        CHECK(counts.providers==0&&counts.outers==0);
    } else {
        CHECK(Result(s,token,10000).status==Status::TimedOut);
        s.Retire(1);
    }
    CHECK(Result(s,token,10001).status==Status::Retired);
    CHECK(Result(s,peer,10001).status==Status::Pending);
    CHECK(s.requests[job.request].working&&s.catalog[a.ticket.index].borrowers==1);
    clock.store(10003); finish.store(true); worker.join();
    if(failure) std::rethrow_exception(failure);
    CHECK(providerValid.load());
    CHECK(Result(s,token,10004).status==Status::Retired&&Result(s,token,10004).dispatched);
    CHECK(!s.requests[job.request].working&&!Input(s,11).busy);
    if(remove) {
        CHECK(s.CleanupOne());
        CHECK(!s.Cell(a.ticket)&&counts.providers==1&&counts.outers==1);
        CHECK(Input(s,22).Ready()&&!counts.releaseLocked);
    }
}

void EpochSaturation() {
    State s; Counts counts; Attachment a(s,counts,11);
    s.catalog[a.ticket.index].inputEpoch=UINT64_MAX-1;
    Resume(a.inner,3); Normal(a.inner,4);
    CHECK(Input(s,11).Ready()&&Input(s,11).epoch==UINT64_MAX);
    auto token=Submit(s,11,7,100); CHECK(token);
    Resume(a.inner,5); Normal(a.inner,6);
    const auto saturated=Input(s,11);
    CHECK(saturated.epoch==UINT64_MAX&&!saturated.resumed&&!saturated.Ready());
    CHECK(Result(s,token,101).status==Status::Retired&&!Result(s,token,101).dispatched);
    CHECK(!Submit(s,11,8,102)&&!s.Claim(102).token);
    Resume(a.inner,7); Normal(a.inner,8);
    CHECK(Input(s,11).epoch==UINT64_MAX&&!Input(s,11).Ready());
}

void QueuedGenerationsShareProvider() {
    State s; Counts counts; Attachment a(s,counts,11);
    auto first=Submit(s,11,1,100),second=Submit(s,11,2,100);
    CHECK(first&&second&&first!=second);
    auto job=s.Claim(101); CHECK(job.token==first);
    CHECK(!s.Claim(101).token&&Input(s,11).busy);
    unsigned sends=0; Succeed(s,job,102,sends);
    job=s.Claim(103); CHECK(job.token==second);
    CHECK(!s.Claim(103).token);
    Succeed(s,job,104,sends);
    CHECK(sends==2&&!Input(s,11).busy);
    CHECK(Result(s,first,105).dispatched&&Result(s,second,105).dispatched);
    CHECK(Result(s,first,105).status==Status::Success&&Result(s,second,105).status==Status::Success);
}

void UndispatchedResourceRecovery() {
    State s; Counts counts; Attachment a(s,counts,11);
    unsigned sends=0;
    const auto first=Submit(s,11,7,100); CHECK(first);
    const auto expired=Result(s,first,10100);
    CHECK(expired.status==Status::TimedOut&&!expired.dispatched&&Input(s,11).Ready());
    const auto retry=Submit(s,11,7,10101); CHECK(retry&&retry!=first);
    GipEnableResult evicted;
    CHECK(!s.Poll(first,evicted,10101));
    auto job=s.Claim(10102); CHECK(job.token==retry);
    Succeed(s,job,10103,sends);
    CHECK(sends==1&&Result(s,retry,10104).status==Status::Success);
    CHECK(Submit(s,11,7,10105)==retry&&!s.Claim(10105).token);

    const auto exhausted=Submit(s,11,8,10200,true); CHECK(exhausted);
    CHECK(Result(s,exhausted,10200).status==Status::Exhausted&&!Result(s,exhausted,10200).dispatched);
    const auto stillExhausted=Submit(s,11,8,10200,true); CHECK(stillExhausted&&stillExhausted!=exhausted);
    CHECK(!s.Poll(exhausted,evicted,10200));
    CHECK(Result(s,stillExhausted,10200).status==Status::Exhausted&&!Result(s,stillExhausted,10200).dispatched);
    const auto recovered=Submit(s,11,8,10201); CHECK(recovered&&recovered!=stillExhausted);
    job=s.Claim(10202); CHECK(job.token==recovered);
    Succeed(s,job,10203,sends);
    CHECK(sends==2&&Result(s,recovered,10204).dispatched);

    // A worker can still be inside its connection check when the logical
    // deadline expires. Its request cell and provider lease cannot be reused.
    const auto checking=Submit(s,11,9,10300); job=s.Claim(10301); CHECK(job.token==checking);
    CHECK(Result(s,checking,20300).status==Status::TimedOut&&!Result(s,checking,20300).dispatched);
    CHECK(Submit(s,11,9,20301)==checking&&Input(s,11).busy);
    unsigned unexpectedSends=0;
    Execute(s,job,[](auto*){return S_OK;},[&](auto*,auto&,auto&){++unexpectedSends;return S_OK;},[]{return 20302ULL;});
    CHECK(!unexpectedSends&&Result(s,checking,20302).status==Status::TimedOut);
    CHECK(!Result(s,checking,20302).dispatched&&!Input(s,11).busy);
    const auto afterReturn=Submit(s,11,9,20303); CHECK(afterReturn&&afterReturn!=checking);
    job=s.Claim(20304); CHECK(job.token==afterReturn);
    Succeed(s,job,20305,sends);
    CHECK(sends==3);

    const auto retiring=Submit(s,11,10,20400,true); CHECK(retiring);
    s.Retire(10);
    CHECK(Submit(s,11,10,20401)==retiring);
    CHECK(Result(s,retiring,20402).status==Status::Retired&&!s.Claim(20402).token);
}

void ResourceRetryRespectsQueueLimit() {
    State s; Counts counts; Attachment a(s,counts,11),b(s,counts,22);
    const auto first=Submit(s,11,7,100); CHECK(first);
    CHECK(Result(s,first,10100).status==Status::TimedOut);
    for(unsigned i=0;i<16;++i) CHECK(Submit(s,22,100+i,10101));
    CHECK(!Submit(s,11,7,10102));
    CHECK(Result(s,first,10102).status==Status::TimedOut&&!Result(s,first,10102).dispatched);
    s.Retire(100);
    const auto retry=Submit(s,11,7,10103); CHECK(retry&&retry!=first);
    GipEnableResult evicted;
    CHECK(!s.Poll(first,evicted,10103));
    for(unsigned i=1;i<16;++i) s.Retire(100+i);
    auto job=s.Claim(10104); CHECK(job.token==retry);
    unsigned sends=0; Succeed(s,job,10105,sends);
    CHECK(sends==1&&!s.Claim(10106).token);
}

void ConcurrentEpochsAndWake() {
    State s; Counts counts; Attachment a(s,counts,11);
    auto token=Submit(s,11,7,100); auto job=s.Claim(101); CHECK(job.token==token);
    std::atomic<bool> sending=false,finish=false;
    std::exception_ptr failure;
    std::jthread worker([&](std::stop_token stop) {
        try {
            Execute(s,job,[](auto*){return S_OK;},[&](auto*,auto&,auto& response) {
                sending.store(true);
                while(!finish.load()&&!stop.stop_requested()) std::this_thread::yield();
                response[0]=7; response[1]=0; return S_OK;
            },[]{return 200ULL;});
        } catch(...) { failure=std::current_exception(); }
    });
    Await([&]{return sending.load();});
    const auto old=Input(s,11);
    for(UINT64 epoch=2;epoch<=5;++epoch) {
        Resume(a.inner,epoch*10); Normal(a.inner,epoch*10+1);
        const auto current=Input(s,11);
        CHECK(current.Ready()&&current.busy&&current.epoch==epoch);
        CHECK(!Submit(s,11,7,110+epoch)&&!s.Claim(110+epoch).token);
    }
    CHECK(Result(s,token,120).status==Status::Retired&&Result(s,token,120).dispatched);
    std::atomic<bool> waiting=false,woke=false;
    std::jthread waiter([&] {
        AcquireSRWLockExclusive(&s.lock);
        waiting.store(true);
        woke.store(SleepConditionVariableSRW(&s.changed,&s.lock,2000,0)!=FALSE);
        ReleaseSRWLockExclusive(&s.lock);
    });
    Await([&]{return waiting.load();});
    finish.store(true); worker.join(); waiter.join();
    if(failure) std::rethrow_exception(failure);
    CHECK(woke.load());
    const auto latest=Input(s,11);
    CHECK(latest.Ready()&&!latest.busy&&latest.epoch==5);
    auto result=Result(s,token,201);
    CHECK(result.status==Status::Retired&&result.provider==old.provider&&result.epoch==old.epoch);
    CHECK(s.Submit(11,7,old.provider,old.epoch,202,false)==token);
    CHECK(!s.Claim(202).token);
    auto next=Submit(s,11,7,202); CHECK(next&&next!=token);
    auto nextJob=s.Claim(203); CHECK(nextJob.token==next);
    unsigned sends=0; Succeed(s,nextJob,204,sends);
    CHECK(sends==1&&Result(s,next,205).status==Status::Success&&Result(s,next,205).epoch==5);
}

struct TraceEnabled {
    TraceEnabled() { trace::SetEnabled(false); trace::SetEnabled(true); }
    ~TraceEnabled() { trace::SetEnabled(false); }
};

void PassiveRawReplay() {
    State s; Counts counts; Attachment a(s,counts,11,false);
    TraceEnabled enabled;
    Resume(a.inner,159794300523ULL);
    Normal(a.inner,159797350861ULL);
    // wgi-usb-active-47744-159649937.log:101,105, unchanged callback payloads.
    auto down=Hex<17>("0000000000008dfdacfe7bfed008020000");
    const auto release=Hex<17>("0000000000008dfdacfe7bfed008000000");
    const auto expectedDown=down;
    CHECK(Message(a.inner,159798960846ULL,custom::GipMessageClass_Command,12,11,17,down.data())==S_OK);
    down.fill(0xee); // Callback storage cannot outlive the call.
    for(unsigned i=0;i<8;++i) Normal(a.inner,159798960847ULL+i);
    CHECK(Message(a.inner,159799072835ULL,custom::GipMessageClass_Command,12,12,17,release.data())==S_OK);
    CHECK(Input(s,11).splitSeen&&Input(s,11).splitTime==159799072835ULL);
    std::array<trace::Record,32> records;
    std::uint64_t dropped=0;
    const auto count=trace::Drain(records.data(),records.size(),dropped);
    CHECK(!dropped&&count>=4);
    unsigned splits=0,resumes=0,normals=0;
    for(std::size_t i=0;i<count;++i) {
        const auto& record=records[i];
        CHECK(record.nativeId==11&&record.provider==a.ticket.serial);
        CHECK(record.kind!=trace::Kind::Decoded&&record.kind!=trace::Kind::Queued&&record.kind!=trace::Kind::Drained);
        if(record.kind==trace::Kind::WgiResume) { ++resumes; CHECK(record.sourceTime==159794300523ULL); }
        if(record.kind!=trace::Kind::WgiMessage) continue;
        CHECK(record.epoch==1);
        if(record.messageClass==1&&record.report==0) { ++normals; CHECK(record.length==46&&record.copied==46); }
        if(record.messageClass==0&&record.report==12) {
            CHECK(splits<2&&record.length==17&&record.copied==17&&!record.truncated);
            const auto& expected=splits==0 ? expectedDown : release;
            CHECK(std::memcmp(record.bytes,expected.data(),17)==0);
            CHECK(record.sequence==(splits==0 ? 11ULL : 12ULL));
            CHECK(record.sourceTime==(splits==0 ? 159798960846ULL : 159799072835ULL));
            ++splits;
        }
    }
    CHECK(resumes==1&&splits==2&&normals==9);
    CHECK(!s.Claim(100).token);
}

void TraceOverflowAndDisabledReadiness() {
    State s; Counts counts; Attachment a(s,counts,11,false);
    trace::SetEnabled(false);
    Resume(a.inner,100); Normal(a.inner,101);
    CHECK(Input(s,11).Ready());
    TraceEnabled enabled;
    ComPtr<custom::IGipGameControllerInputSink> sink;
    CHECK(a.inner->QueryInterface(IID_PPV_ARGS(&sink))==S_OK);
    std::array<BYTE,46> normal{};
    for(unsigned i=0;i<5000;++i) CHECK(sink->OnMessageReceived(102+i,custom::GipMessageClass_LowLatency,0,
        static_cast<BYTE>(i),46,normal.data())==S_OK);
    Suspend(a.inner,6000); Resume(a.inner,6001); Normal(a.inner,6002);
    CHECK(Input(s,11).Ready()&&Input(s,11).epoch==2);
    std::array<BYTE,80> oversized; oversized.fill(0x5a);
    CHECK(Message(a.inner,6003,custom::GipMessageClass_StandardLatency,31,0,80,oversized.data())==S_OK);
    CHECK(Message(a.inner,6004,custom::GipMessageClass_LowLatency,0,0,46,nullptr)==E_POINTER);
    std::array<trace::Record,64> records;
    std::uint64_t dropped=0,totalDropped=0;
    bool sawSuspend=false,sawResume=false,sawOversized=false,sawNull=false;
    std::size_t drained=0,count;
    do {
        count=trace::Drain(records.data(),records.size(),dropped);
        totalDropped+=dropped; drained+=count;
        for(std::size_t i=0;i<count;++i) {
            const auto& record=records[i];
            sawSuspend|=record.kind==trace::Kind::WgiSuspend&&record.sourceTime==6000;
            sawResume|=record.kind==trace::Kind::WgiResume&&record.sourceTime==6001;
            if(record.sourceTime==6003) {
                CHECK(record.length==80&&record.copied==64&&record.truncated);
                for(unsigned j=0;j<64;++j) CHECK(record.bytes[j]==0x5a);
                sawOversized=true;
            }
            if(record.sourceTime==6004) { CHECK(record.length==46&&!record.copied&&record.truncated); sawNull=true; }
        }
    } while(count);
    CHECK(totalDropped>0&&drained<5005);
    CHECK(sawSuspend&&sawResume&&sawOversized&&sawNull);
    CHECK(Input(s,11).normalTime==6002);
}

int main() {
    try {
        Identifiers(); CommandsAndTimeout(); QueuesAndExhaustion(); LateCatalogAndCrossRemoval();
        OldCleanupAndNewPending(); AggregationAndLimits(); FactoryMetadata(); ApiAndContention();
        CapturedReadinessAndRearm(); NativeImplicitResumeOrder(); ColdStartAndShapes(); SuspendBeforeDispatch();
        ConcurrentRetirement(); ConcurrentRetirement(true); ConcurrentEpochsAndWake();
        EpochSaturation(); QueuedGenerationsShareProvider(); UndispatchedResourceRecovery();
        ResourceRetryRespectsQueueLimit(); PassiveRawReplay(); TraceOverflowAndDisabledReadiness();
        std::printf("PASS %u checks; offline production state/runner/aggregation/readiness/trace; no WGI activation\n",checks.load());
        return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what());return 1; }
}
