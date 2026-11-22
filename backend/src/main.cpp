#include "hetero/http_server.hpp"
#include <iostream>
int main()
{
    try
    {
        hetero::Controller controller;
        hetero::run_http_server(controller, 8080);
    }
    catch (const std::exception& e)
    {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
