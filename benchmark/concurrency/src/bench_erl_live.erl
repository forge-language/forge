%% Erlang concurrency-primitive benchmark, hold-live variant.
%%
%% The direct counterpart to bench_forge.fg and bench_go_live.go: spawn N
%% processes, confirm all N are alive, sample peak memory at that instant,
%% then release them. Forge borrows its process/supervisor/mailbox vocabulary
%% and its reduction-budget preemption from this runtime, so BEAM is the
%% reference Forge's concurrency claims should be read against.
%%
%% erlang:memory(total) is used rather than VmHWM because the BEAM allocates
%% its carriers up front and holds them: OS-level RSS reports the emulator's
%% arenas, not what the processes cost. Both figures are printed so the
%% comparison against Forge's maxrss can be made on either basis.
-module(bench_erl_live).
-export([main/1]).

main([NStr]) ->
    N = list_to_integer(NStr),
    Parent = self(),
    T0 = erlang:monotonic_time(microsecond),
    Pids = [spawn(fun() -> worker(Parent) end) || _ <- lists:seq(1, N)],
    %% Wait until every process has reported itself alive, so "N live" is a
    %% checked fact and not an assumption about scheduler timing.
    ok = collect(N, ready),
    T1 = erlang:monotonic_time(microsecond),
    BeamTotal = erlang:memory(total),
    ProcMem = erlang:memory(processes),
    RssKb = rss_kb(),
    [P ! go || P <- Pids],
    ok = collect(N, done),
    T2 = erlang:monotonic_time(microsecond),
    io:format("live=~w spawn_ms=~.3f total_ms=~.3f beam_total_kb=~w "
              "proc_mem_kb=~w peak_rss_kb=~w bytes_per_task=~.1f~n",
              [N, (T1 - T0) / 1000, (T2 - T0) / 1000,
               BeamTotal div 1024, ProcMem div 1024, RssKb,
               ProcMem / N]),
    halt(0).

worker(Parent) ->
    Parent ! ready,
    receive go -> Parent ! done end.

collect(0, _Tag) -> ok;
collect(N, Tag) ->
    receive Tag -> collect(N - 1, Tag) end.

%% Peak resident set, for comparison against the other runtimes' maxrss.
rss_kb() ->
    {ok, Bin} = file:read_file("/proc/self/status"),
    [Line | _] = [L || L <- binary:split(Bin, <<"\n">>, [global]),
                       binary:match(L, <<"VmHWM:">>) =/= nomatch],
    [_, V | _] = [X || X <- binary:split(Line, [<<" ">>, <<"\t">>], [global]),
                       X =/= <<>>],
    binary_to_integer(V).
