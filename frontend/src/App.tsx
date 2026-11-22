import { useCallback, useEffect, useState } from 'react';

type Worker = {
    id: number;
    state: string;
    speed_factor: number;
    completed_tiles: number;
    busy_us: number;
    task_id: number | null;
};
type Event = { time_us: number; type: string; task_id: number; worker_id: number };
type State = {
    job_state: string;
    policy: string;
    sim_time_us: number;
    matrix_size: number;
    tile_size: number;
    worker_count: number;
    total_tasks: number;
    completed_tasks: number;
    progress: number;
    worker_utilization: number;
    workers: Worker[];
    events: Event[];
};
const empty: State = {
    job_state: 'idle',
    policy: 'predicted',
    sim_time_us: 0,
    matrix_size: 128,
    tile_size: 32,
    worker_count: 3,
    total_tasks: 0,
    completed_tasks: 0,
    progress: 0,
    worker_utilization: 0,
    workers: [],
    events: [],
};
async function api(path: string, body?: unknown, method?: 'GET' | 'POST') {
    const requestMethod = method ?? (body === undefined ? 'GET' : 'POST');

    const response = await fetch(path, {
        method: requestMethod,
        headers: {
            'Content-Type': 'application/json',
        },
        body: body === undefined ? undefined : JSON.stringify(body),
    });

    const data = await response.json();

    if (!response.ok) {
        throw new Error(data.error ?? 'API error');
    }

    return data;
}
export default function App() {
    const [s, setS] = useState<State>(empty);
    const [matrix, setMatrix] = useState(128);
    const [tile, setTile] = useState(32);
    const [workers, setWorkers] = useState(3);
    const [policy, setPolicy] = useState('predicted');
    const [error, setError] = useState('');
    const refresh = useCallback(async () => {
        try {
            setS(await api('/api/state'));
            setError('');
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Backend unavailable');
        }
    }, []);
    useEffect(() => {
        refresh();
        const t = setInterval(refresh, 350);
        return () => clearInterval(t);
    }, [refresh]);
    const action = async (path: string, body?: unknown) => {
        try {
            await api(path, body, 'POST');
            await refresh();
            setError('');
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Request failed');
        }
    };
    const configure = () =>
        action('/api/configure', {
            matrix_size: matrix,
            tile_size: tile,
            worker_count: workers,
            spi_mbps: 10,
            wall_delay_ms: 4,
        });
    const changePolicy = (p: string) => {
        setPolicy(p);
        action('/api/policy', { policy: p });
    };
    const pct = Math.round(s.progress * 100);
    const fmt = (v: number) => (v >= 1000 ? (v / 1000).toFixed(2) + ' ms' : v.toFixed(0) + ' µs');
    return (
        <main className="shell">
            <header>
                <div>
                    <div className="eyebrow">HETEROGENEOUS COMPUTING PLATFORM</div>
                    <h1>HeteroCompute</h1>
                    <p>Централизованное управление распределённым вычислением</p>
                </div>
                <div className="backend">
                    <span className="dot" /> SIMULATION BACKEND <small></small>
                </div>
            </header>
            {error && <div className="error">{error}. Проверьте, что C++ backend запущен.</div>}
            <nav>
                <a className="active">Dashboard</a>
                <a>Nodes</a>
                <a>Scheduler</a>
                <a>Experiments</a>
                <a>Analytics</a>
            </nav>
            <section className="metrics">
                <Metric label="JOB STATE" value={s.job_state.toUpperCase()} />
                <Metric
                    label="PROGRESS"
                    value={`${pct}%`}
                    detail={`${s.completed_tasks} / ${s.total_tasks} tiles`}
                />
                <Metric label="SIMULATION TIME" value={fmt(s.sim_time_us)} />
                <Metric
                    label="WORKER UTILIZATION"
                    value={`${(s.worker_utilization * 100).toFixed(1)}%`}
                />
            </section>
            <section className="layout">
                <article className="panel">
                    <div className="panel-head">
                        <div>
                            <h2>Executor nodes</h2>
                            <p>Активные исполнительные узлы</p>
                        </div>
                    </div>
                    <div className="nodes">
                        {s.workers.map((w) => (
                            <div className="node" key={w.id}>
                                <div className="node-top">
                                    <b>WORKER N{w.id + 1}</b>
                                    <span className={`state ${w.state}`}>
                                        {w.state.replace('_', ' ')}
                                    </span>
                                </div>
                                <div className="speed">
                                    Performance factor <b>{w.speed_factor.toFixed(2)}×</b>
                                </div>
                                <div className="node-stats">
                                    <span>Completed tiles</span>
                                    <b>{w.completed_tiles}</b>
                                </div>
                                <div className="node-stats">
                                    <span>Compute busy</span>
                                    <b>{fmt(w.busy_us)}</b>
                                </div>
                                <button
                                    className="link"
                                    onClick={() =>
                                        action('/api/worker', {
                                            id: w.id,
                                            enabled: w.state === 'disabled',
                                        })
                                    }
                                >
                                    {w.state === 'disabled' ? 'Enable node' : 'Disable when idle'}
                                </button>
                            </div>
                        ))}
                    </div>
                </article>
                <article className="panel config">
                    <div className="panel-head">
                        <div>
                            <h2>Workload & scheduler</h2>
                            <p>Параметры вычислительного эксперимента</p>
                        </div>
                    </div>
                    <label>
                        Matrix dimension
                        <select value={matrix} onChange={(e) => setMatrix(Number(e.target.value))}>
                            <option value={64}>64 × 64</option>
                            <option value={128}>128 × 128</option>
                            <option value={256}>256 × 256</option>
                        </select>
                    </label>
                    <label>
                        Tile dimension
                        <select value={tile} onChange={(e) => setTile(Number(e.target.value))}>
                            <option value={16}>16 × 16</option>
                            <option value={32}>32 × 32</option>
                            <option value={64}>64 × 64</option>
                        </select>
                    </label>
                    <label>
                        Available workers
                        <select
                            value={workers}
                            onChange={(e) => setWorkers(Number(e.target.value))}
                        >
                            <option value={1}>1 worker</option>
                            <option value={2}>2 workers</option>
                            <option value={3}>3 workers</option>
                        </select>
                    </label>
                    <label>
                        Scheduling policy
                        <select value={policy} onChange={(e) => changePolicy(e.target.value)}>
                            <option value="predicted">Predicted</option>
                            <option value="least_loaded">Least loaded</option>
                            <option value="round_robin">Round robin</option>
                        </select>
                    </label>
                    <div className="buttons">
                        <button onClick={configure}>Apply configuration</button>
                        <button className="secondary" onClick={() => action('/api/reset')}>
                            Reset
                        </button>
                    </div>
                    <div className="buttons">
                        <button onClick={() => action('/api/start')}>Start</button>
                        <button className="secondary" onClick={() => action('/api/pause')}>
                            Pause
                        </button>
                        <button className="secondary" onClick={() => action('/api/resume')}>
                            Resume
                        </button>
                    </div>
                </article>
            </section>
            <section className="panel timeline">
                <div className="panel-head">
                    <div>
                        <h2>Execution trace</h2>
                        <p>Журнал событий дискретно-событийной модели</p>
                    </div>
                    <span className="muted">Virtual time · µs</span>
                </div>
                <div className="table-wrap">
                    <table>
                        <thead>
                            <tr>
                                <th>TIME</th>
                                <th>EVENT</th>
                                <th>TASK</th>
                                <th>WORKER</th>
                            </tr>
                        </thead>
                        <tbody>
                            {[...s.events].reverse().map((e, i) => (
                                <tr key={`${e.time_us}-${e.type}-${i}`}>
                                    <td>{e.time_us.toFixed(1)}</td>
                                    <td className="event">{e.type}</td>
                                    <td>{e.task_id}</td>
                                    <td>N{e.worker_id + 1}</td>
                                </tr>
                            ))}
                            {s.events.length === 0 && (
                                <tr>
                                    <td colSpan={4} className="empty">
                                        Запустите эксперимент, чтобы увидеть трассу событий
                                    </td>
                                </tr>
                            )}
                        </tbody>
                    </table>
                </div>
            </section>
            <footer>
                HeteroCompute · v0.2 · Discrete-event simulation · Results are model estimates, not
                hardware measurements.
            </footer>
        </main>
    );
}
function Metric({ label, value, detail }: { label: string; value: string; detail?: string }) {
    return (
        <div className="metric">
            <span>{label}</span>
            <strong>{value}</strong>
            {detail && <small>{detail}</small>}
        </div>
    );
}
