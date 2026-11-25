#include "mini_webserver.h"
#include "test.h"

#include <string.h>

static mw_route_t *match(mw_route_t *routes, size_t count, const char *path) {
    return mw_proxy_match_route(routes, count,
                                (mw_slice_t){(const unsigned char *)path, strlen(path)});
}

bool test_proxy_match_route(void) {
    static mw_route_t routes[3];
    const char *prefixes[] = {"/api/", "/api/admin/", "/service"};
    for (size_t i = 0; i < 3; i++) {
        memcpy(routes[i].prefix, prefixes[i], strlen(prefixes[i]) + 1U);
    }
    CHECK(match(routes, 3, "/api/") == &routes[0] && match(routes, 3, "/api/x") == &routes[0]);
    CHECK(match(routes, 3, "/api") == NULL && match(routes, 3, "/apix") == NULL);
    CHECK(match(routes, 3, "/api/admin/users") == &routes[1]); /* longest prefix wins */
    CHECK(match(routes, 3, "/api/admin") == &routes[0]);
    CHECK(match(routes, 3, "/service") == &routes[2] &&
          match(routes, 3, "/service/x") == &routes[2]);
    CHECK(match(routes, 3, "/service-other") == NULL && match(routes, 3, "/servicex") == NULL);
    CHECK(match(routes, 3, "/%61pi/x") == NULL); /* raw path, never decoded */
    CHECK(match(routes, 3, "/api/%61dmin/x") == &routes[0] &&
          match(routes, 3, "/api%2Fadmin/") == NULL);
    CHECK(match(routes, 3, "/service/") == &routes[2] && match(routes, 3, "/servic") == NULL);
    CHECK(match(routes, 3, "/") == NULL && match(routes, 0, "/api/") == NULL);
    return true;
}

bool test_proxy_round_robin(void) {
    static mw_route_t route;
    CHECK(pthread_mutex_init(&route.selection_lock, NULL) == 0);
    route.backend_count = 3;
    for (size_t i = 0; i < 7; i++) {
        CHECK(mw_proxy_next_backend(&route) == &route.backends[i % 3]);
    }
    (void)pthread_mutex_destroy(&route.selection_lock);
    return true;
}
