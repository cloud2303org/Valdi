#include "valdi_nsh_transport.h"
#include <atomic>
#include <cassert>
#include <cstddef>
struct State { std::atomic<unsigned> wakes{0}, releases{0}; };
static void wake(void *p) { static_cast<State *>(p)->wakes.fetch_add(1); }
static void release(void *p) { static_cast<State *>(p)->releases.fetch_add(1); }
static_assert(sizeof(NshTransportBytes)==2*sizeof(void *));
#if INTPTR_MAX == INT64_MAX
static_assert(sizeof(NshTransportCallbacks)==40);
static_assert(sizeof(NshTransportHttpOp)==80);
static_assert(sizeof(NshTransportHttpEvent)==88);
static_assert(sizeof(NshTransportSocketOp)==64);
static_assert(sizeof(NshTransportSocketEvent)==40);
#endif
int main() {
    State state;
    NshTransportCallbacks callbacks{NSH_TRANSPORT_ABI_VERSION,sizeof(NshTransportCallbacks),&state,wake,release};
    NshTransport *transport=nsh_transport_create(&callbacks);
    assert(transport);
    NshTransportHttpOp cancel{}; cancel.id=1; cancel.op=NSH_TRANSPORT_HTTP_CANCEL;
    assert(nsh_transport_http(transport,&cancel)==0);
    nsh_transport_stop(transport);
    assert(state.releases==1);
    assert(nsh_transport_http(transport,&cancel)==-1);
    nsh_transport_stop(transport);
    nsh_transport_destroy(transport);
    assert(state.releases==1);
}
