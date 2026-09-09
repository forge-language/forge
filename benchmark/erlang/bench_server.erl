%% Raw gen_tcp HTTP server for the cross-language benchmark.
%%
%% Deliberately not Cowboy/Phoenix: those add routing, header maps and a
%% request pipeline, and would be measured against Forge's http_serve_*, which
%% does none of that. This is the BEAM counterpart to benchmark/go's MODE=raw
%% and to bench_server.fg -- an acceptor pool writing one fixed response --
%% so the number reflects the runtime rather than a framework.
%%
%% The response is byte-identical to every other server in the suite (70
%% bytes, two headers); benchmark/xlang/probe_wire.py refuses to time a
%% server whose framing differs, so parity here is enforced, not assumed.
-module(bench_server).
-export([main/1]).

-define(RESP, <<"HTTP/1.1 200 OK\r\nContent-Length: 12\r\n"
                "Connection: close\r\n\r\nHello, World">>).

main([PortStr]) ->
    Port = list_to_integer(PortStr),
    {ok, L} = gen_tcp:listen(Port, [binary,
                                    {active, false},
                                    {reuseaddr, true},
                                    {backlog, 8192},
                                    {nodelay, true},
                                    {packet, raw}]),
    %% One acceptor per scheduler: a single acceptor process serialises the
    %% accept() call and would cap throughput below what the runtime can do.
    N = erlang:system_info(schedulers_online),
    [spawn_link(fun() -> accept_loop(L) end) || _ <- lists:seq(1, N)],
    io:format("Erlang benchmark server on port ~w (~w acceptors)~n", [Port, N]),
    receive stop -> ok end.

accept_loop(L) ->
    case gen_tcp:accept(L) of
        {ok, S} ->
            spawn(fun() -> handle(S) end),
            accept_loop(L);
        {error, closed} -> ok;
        {error, _} -> accept_loop(L)
    end.

handle(S) ->
    %% One recv is enough: every request in this suite is a single small GET
    %% that arrives in one segment. A short read would show up as a wire-probe
    %% mismatch or a load-generator error, not as a silently fast number.
    case gen_tcp:recv(S, 0, 5000) of
        {ok, _} -> gen_tcp:send(S, ?RESP);
        {error, _} -> ok
    end,
    gen_tcp:close(S).
