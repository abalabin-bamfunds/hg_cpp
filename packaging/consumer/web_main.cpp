#include <hg/web/http_client.hpp>
#include <hg/web/websocket_client.hpp>

int main() {
    const auto http = hg::web::http_client_impl();
    const auto websocket = hg::web::websocket_client_impl();
    return http && websocket ? 0 : 1;
}
