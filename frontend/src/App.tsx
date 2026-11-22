import { useCallback, useEffect, useMemo, useState } from 'react';
import type { ReactNode } from 'react';
import './style.css';

type Worker = {
    id: number;
    state: string;
    speed_factor: number;
    completed_tiles: number;
    busy_us: number;
    task_id: number | null;
    ready_asserted?: boolean;
};

type Event = {
    time_us: number;
    type: string;
    task_id: number;
    worker_id: number;
};

type RunSummary = {
    run_id: number;
    policy: string;
    matrix_size: number;
    tile_size: number;
    worker_count: number;
    total_tasks: number;
    sim_time_us: number;
    worker_utilization: number;
    relative_frobenius_error: number;
    passed: boolean;
    workers: { worker_id: number; completed_tiles: number; busy_us: number }[];
};

type State = {
    backend_mode?: string;
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
    tasks: { id: number; row: number; col: number; state: string; worker_id: number | null }[];
    run_history?: RunSummary[];
    numerical_validation?: {
        done: boolean;
        computed_tiles: number;
        total_tiles: number;
        relative_frobenius_error: number | null;
        passed: boolean;
    };
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
    tasks: [],
    run_history: [],
};

async function api(path: string, body?: unknown) {
    const response = await fetch(path, {
        method: body === undefined ? 'GET' : 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: body === undefined ? undefined : JSON.stringify(body),
    });

    const data = await response.json();
    if (!response.ok) throw new Error(data.error ?? 'Ошибка API');
    return data;
}

export default function App() {
    const [s, setS] = useState<State>(empty);
    const [matrix, setMatrix] = useState(128);
    const [tile, setTile] = useState(32);
    const [workers, setWorkers] = useState(3);
    const [policy, setPolicy] = useState('predicted');
    const [error, setError] = useState('');
    const [page, setPage] = useState('dashboard');
    const [busy, setBusy] = useState(false);

    const refresh = useCallback(async () => {
        try {
            const next = await api('/api/state');
            setS(next);
            setError('');
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Сервер вычислений недоступен');
        }
    }, []);

    useEffect(() => {
        refresh();
        const timer = window.setInterval(refresh, 500);
        return () => window.clearInterval(timer);
    }, [refresh]);

    const action = async (path: string, body?: unknown) => {
        if (busy) return;
        setBusy(true);
        try {
            await api(path, body ?? {});
            await refresh();
            setError('');
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Не удалось выполнить операцию');
        } finally {
            setBusy(false);
        }
    };

    const configure = async () => {
        if (busy) return;
        setBusy(true);
        try {
            await api('/api/configure', {
                matrix_size: matrix,
                tile_size: tile,
                worker_count: workers,
                spi_mbps: 10,
                wall_delay_ms: 4,
            });
            await api('/api/policy', { policy });
            await refresh();
            setError('');
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Не удалось применить конфигурацию');
        } finally {
            setBusy(false);
        }
    };

    const exportLog = async () => {
        try {
            const response = await fetch('/api/log.txt');
            if (!response.ok) throw new Error('Не удалось получить журнал событий');
            const blob = await response.blob();
            const url = URL.createObjectURL(blob);
            const link = document.createElement('a');
            link.href = url;
            link.download = 'журнал_вычислений.txt';
            document.body.appendChild(link);
            link.click();
            link.remove();
            window.setTimeout(() => URL.revokeObjectURL(url), 1000);
            setError('');
        } catch (e) {
            setError(e instanceof Error ? e.message : 'Ошибка экспорта журнала');
        }
    };

    const pct = Math.max(0, Math.min(100, Math.round((s.progress ?? 0) * 100)));
    const fmt = (value: number) =>
        value >= 1000 ? `${(value / 1000).toFixed(2)} мс` : `${value.toFixed(0)} мкс`;

    const activeWorkers = s.workers.filter((w) => w.state !== 'FAILED').length;
    const pageTitle = ({
        dashboard: 'Обзор системы',
        nodes: 'Вычислительные узлы',
        scheduler: 'Диспетчеризация',
        experiments: 'Параметры эксперимента',
        analytics: 'Аналитика вычислений',
    } as Record<string, string>)[page];

    const latestRuns = useMemo(
        () => [...(s.run_history ?? [])].slice(-3).reverse(),
        [s.run_history],
    );

    return (
        <main className="app-shell">
            <header className="topbar">
                <div className="brand">
                    <div className="brand-mark" aria-hidden="true">
                        <span />
                        <span />
                        <span />
                    </div>
                    <div className="brand-copy">
                        <div className="brand-name">HETERO<span>COMPUTE</span></div>
                        <div className="brand-caption">ПЛАТФОРМА РАСПРЕДЕЛЁННЫХ ВЫЧИСЛЕНИЙ</div>
                    </div>
                </div>

                <div className="topbar-status">
                    <span className={`status-indicator ${error ? 'offline' : 'online'}`} />
                    <span>{error ? 'НЕТ СВЯЗИ С СЕРВЕРОМ' : 'СЕРВЕР ДОСТУПЕН'}</span>
                    <i />
                    <span className="mode-label">
                        {s.backend_mode === 'hardware' ? 'АППАРАТНЫЙ РЕЖИМ' : 'МОДЕЛИРОВАНИЕ'}
                    </span>
                </div>
            </header>

            <nav className="main-nav" aria-label="Основная навигация">
                {[
                    { id: 'dashboard', label: 'Обзор' },
                    { id: 'nodes', label: 'Узлы' },
                    { id: 'scheduler', label: 'Диспетчеризация' },
                    { id: 'experiments', label: 'Эксперименты' },
                    { id: 'analytics', label: 'Аналитика' },
                ].map((item) => (
                    <button
                        key={item.id}
                        className={`nav-item ${page === item.id ? 'selected' : ''}`}
                        onClick={() => setPage(item.id)}
                        type="button"
                    >
                        {item.label}
                    </button>
                ))}
                <div className="nav-spacer" />
                <div className="nav-job-state">
                    <span className={`job-dot ${s.job_state}`} />
                    {jobStateName(s.job_state)}
                </div>
            </nav>

            {error && (
                <div className="connection-alert" role="alert">
                    <span className="alert-symbol">!</span>
                    <div>
                        <strong>Не удалось связаться с вычислительным сервером</strong>
                        <p>{error}. Проверьте состояние backend и сетевое подключение.</p>
                    </div>
                </div>
            )}

            <div className="page-heading">
                <div>
                    <div className="eyebrow">СИСТЕМА / {page.toUpperCase()}</div>
                    <h1>{pageTitle}</h1>
                </div>
                <div className="page-meta">
                    <span>МАТРИЦА {s.matrix_size} × {s.matrix_size}</span>
                    <span className="meta-divider" />
                    <span>БЛОК {s.tile_size} × {s.tile_size}</span>
                </div>
            </div>

            {page === 'dashboard' && (
                <>
                    <section className="kpi-grid">
                        <Metric
                            label="СОСТОЯНИЕ ЗАДАНИЯ"
                            value={jobStateName(s.job_state)}
                            detail={s.job_state === 'running' ? 'Вычисление выполняется' : 'Текущий эксперимент'}
                            accent={s.job_state === 'running' ? 'blue' : 'neutral'}
                        />
                        <Metric
                            label="ВЫПОЛНЕНИЕ"
                            value={`${pct}%`}
                            detail={`${s.completed_tasks} из ${s.total_tasks} блоков`}
                            accent="blue"
                            progress={pct}
                        />
                        <Metric
                            label="МОДЕЛЬНОЕ ВРЕМЯ"
                            value={fmt(s.sim_time_us)}
                            detail="От начала текущего задания"
                        />
                        <Metric
                            label="СРЕДНЯЯ ЗАГРУЗКА"
                            value={`${(s.worker_utilization * 100).toFixed(1)}%`}
                            detail={`${activeWorkers} доступных исполнителя`}
                            accent="green"
                        />
                    </section>

                    <section className="dashboard-grid">
                        <article className="surface workers-surface">
                            <PanelTitle
                                eyebrow="ИСПОЛНИТЕЛЬНЫЙ КОНТУР"
                                title="Состояние узлов"
                                description="Текущая загрузка и состояние вычислительных исполнителей"
                                aside={`${s.workers.length} УЗЛА`}
                            />
                            <div className="worker-grid">
                                {s.workers.map((w) => {
                                    const maxBusy = Math.max(...s.workers.map((x) => x.busy_us), 1);
                                    return (
                                        <div className="worker-card" key={w.id}>
                                            <div className="worker-card-head">
                                                <div className="worker-id">
                                                    <span className="worker-icon">N{w.id + 1}</span>
                                                    <div>
                                                        <strong>Узел {w.id + 1}</strong>
                                                        <small>ИСПОЛНИТЕЛЬ</small>
                                                    </div>
                                                </div>
                                                <StatePill state={w.state} />
                                            </div>
                                            <div className="worker-performance">
                                                <span>Коэффициент производительности</span>
                                                <strong>{w.speed_factor.toFixed(2)}×</strong>
                                            </div>
                                            <div className="worker-load">
                                                <div className="load-label">
                                                    <span>Относительная вычислительная нагрузка</span>
                                                    <strong>{fmt(w.busy_us)}</strong>
                                                </div>
                                                <div className="progress-track">
                                                    <div
                                                        className="progress-fill"
                                                        style={{ width: `${(w.busy_us / maxBusy) * 100}%` }}
                                                    />
                                                </div>
                                            </div>
                                            <div className="worker-footer">
                                                <span>Завершено блоков</span>
                                                <strong>{w.completed_tiles}</strong>
                                                <span className="worker-current">
                                                    {w.task_id === null ? 'Нет активной задачи' : `Задача №${w.task_id}`}
                                                </span>
                                            </div>
                                        </div>
                                    );
                                })}
                                {s.workers.length === 0 && (
                                    <div className="empty-state compact">Данные об исполнителях пока не получены.</div>
                                )}
                            </div>
                        </article>

                        <article className="surface config-surface">
                            <PanelTitle
                                eyebrow="ПАРАМЕТРЫ ЗАДАНИЯ"
                                title="Быстрая настройка"
                                description="Конфигурация текущего вычислительного эксперимента"
                            />
                            <div className="form-grid">
                                <Field label="Размер матрицы">
                                    <select value={matrix} onChange={(e) => setMatrix(Number(e.target.value))}>
                                        <option value={64}>64 × 64</option>
                                        <option value={128}>128 × 128</option>
                                        <option value={256}>256 × 256</option>
                                    </select>
                                </Field>
                                <Field label="Размер выходного блока">
                                    <select value={tile} onChange={(e) => setTile(Number(e.target.value))}>
                                        <option value={16}>16 × 16</option>
                                        <option value={32}>32 × 32</option>
                                        <option value={64}>64 × 64</option>
                                    </select>
                                </Field>
                                <Field label="Число исполнителей">
                                    <select value={workers} onChange={(e) => setWorkers(Number(e.target.value))}>
                                        <option value={1}>1 узел</option>
                                        <option value={2}>2 узла</option>
                                        <option value={3}>3 узла</option>
                                    </select>
                                </Field>
                                <Field label="Алгоритм диспетчеризации">
                                    <select value={policy} onChange={(e) => setPolicy(e.target.value)}>
                                        <option value="predicted">По прогнозируемому времени</option>
                                        <option value="least_loaded">По текущей загрузке</option>
                                        <option value="round_robin">Циклический</option>
                                    </select>
                                </Field>
                            </div>
                            <div className="action-row">
                                <button className="button button-primary" onClick={configure} disabled={busy}>
                                    Применить параметры
                                </button>
                                <button className="button button-quiet" onClick={() => action('/api/reset')} disabled={busy}>
                                    Сбросить
                                </button>
                            </div>
                            <div className="action-row run-actions">
                                <button className="button button-run" onClick={() => action('/api/start')} disabled={busy}>
                                    <span className="button-icon">▶</span> Запустить
                                </button>
                                <button className="button button-secondary" onClick={() => action('/api/pause')} disabled={busy}>
                                    Пауза
                                </button>
                                <button className="button button-secondary" onClick={() => action('/api/resume')} disabled={busy}>
                                    Продолжить
                                </button>
                            </div>
                            <div className="config-note">
                                <span className="note-mark">i</span>
                                Матрицы генерируются детерминированно. При одинаковых параметрах исходные данные совпадают.
                            </div>
                        </article>
                    </section>

                    <section className="surface log-surface">
                        <PanelTitle
                            eyebrow="СИСТЕМНЫЕ СОБЫТИЯ"
                            title="Журнал выполнения"
                            description="Последовательность событий текущего вычислительного задания"
                            aside={
                                <button className="button button-secondary button-small" onClick={exportLog}>
                                    <span className="download-icon">↓</span> Выгрузить журнал .txt
                                </button>
                            }
                        />
                        <EventTable events={s.events} fmt={fmt} />
                    </section>
                </>
            )}

            {page === 'nodes' && (
                <section className="surface page-surface">
                    <PanelTitle
                        eyebrow="ИСПОЛНИТЕЛЬНЫЙ КОНТУР"
                        title="Вычислительные узлы"
                        description="Состояние, производительность и накопленная нагрузка каждого исполнителя"
                    />
                    <div className="worker-grid worker-grid-large">
                        {s.workers.map((w) => (
                            <div className="worker-card" key={w.id}>
                                <div className="worker-card-head">
                                    <div className="worker-id">
                                        <span className="worker-icon">N{w.id + 1}</span>
                                        <div><strong>Узел {w.id + 1}</strong><small>STM32 / МОДЕЛЬ ИСПОЛНИТЕЛЯ</small></div>
                                    </div>
                                    <StatePill state={w.state} />
                                </div>
                                <div className="node-detail-grid">
                                    <DataPoint label="Производительность" value={`${w.speed_factor.toFixed(2)}×`} />
                                    <DataPoint label="Завершено блоков" value={String(w.completed_tiles)} />
                                    <DataPoint label="Вычислительное время" value={fmt(w.busy_us)} />
                                    <DataPoint label="Текущая задача" value={w.task_id === null ? '—' : `№${w.task_id}`} />
                                    <DataPoint label="READY#" value={w.ready_asserted ? 'Активен · низкий' : 'Неактивен · высокий'} />
                                </div>
                            </div>
                        ))}
                        {s.workers.length === 0 && <div className="empty-state">Нет данных об исполнительных узлах.</div>}
                    </div>
                    <div className="info-strip">
                        <strong>Примечание к модели</strong>
                        <span>Обмен по общей шине SPI выполняется последовательно. Вычисления на разных исполнителях могут перекрываться по времени.</span>
                    </div>
                </section>
            )}

            {page === 'scheduler' && (
                <>
                    <section className="kpi-grid">
                        <Metric label="ТЕКУЩАЯ ПОЛИТИКА" value={policyName(s.policy)} detail="Алгоритм выбора исполнителя" />
                        <Metric label="ОЖИДАЮТ НАЗНАЧЕНИЯ" value={String(s.tasks.filter((t) => t.state === 'queued').length)} />
                        <Metric label="В РАБОТЕ" value={String(s.tasks.filter((t) => !['queued', 'completed'].includes(t.state)).length)} />
                        <Metric label="ЗАВЕРШЕНО" value={`${s.completed_tasks} / ${s.total_tasks}`} />
                    </section>
                    <section className="surface page-surface">
                        <PanelTitle
                            eyebrow="ОЧЕРЕДЬ ЗАДАНИЙ"
                            title="Распределение вычислительных блоков"
                            description="Каждая задача соответствует одному блоку результирующей матрицы"
                        />
                        <div className="table-container">
                            <table>
                                <thead><tr><th>ЗАДАЧА</th><th>СТРОКА БЛОКА</th><th>СТОЛБЕЦ БЛОКА</th><th>СОСТОЯНИЕ</th><th>ИСПОЛНИТЕЛЬ</th></tr></thead>
                                <tbody>
                                {s.tasks.map((t) => (
                                    <tr key={t.id}>
                                        <td className="mono">#{t.id}</td>
                                        <td>{t.row}</td>
                                        <td>{t.col}</td>
                                        <td><TaskPill state={t.state} /></td>
                                        <td>{t.worker_id === null ? '—' : `N${t.worker_id + 1}`}</td>
                                    </tr>
                                ))}
                                {s.tasks.length === 0 && <tr><td colSpan={5} className="table-empty">Задачи появятся после запуска эксперимента.</td></tr>}
                                </tbody>
                            </table>
                        </div>
                    </section>
                    <section className="surface page-surface">
                        <PanelTitle eyebrow="СИСТЕМНЫЕ СОБЫТИЯ" title="Журнал диспетчеризации" description="Назначение блоков и переходы состояний" />
                        <EventTable events={s.events} fmt={fmt} />
                    </section>
                </>
            )}

            {page === 'experiments' && (
                <section className="surface page-surface experiment-page">
                    <PanelTitle
                        eyebrow="НАСТРОЙКА И ЗАПУСК"
                        title="Параметры вычислительного эксперимента"
                        description="Задайте размерность задачи, конфигурацию исполнителей и алгоритм распределения нагрузки"
                    />
                    <div className="experiment-form">
                        <Field label="Размер исходных матриц">
                            <select value={matrix} onChange={(e) => setMatrix(Number(e.target.value))}>
                                <option value={64}>64 × 64</option><option value={128}>128 × 128</option><option value={256}>256 × 256</option>
                            </select>
                        </Field>
                        <Field label="Размер выходного блока">
                            <select value={tile} onChange={(e) => setTile(Number(e.target.value))}>
                                <option value={16}>16 × 16</option><option value={32}>32 × 32</option><option value={64}>64 × 64</option>
                            </select>
                        </Field>
                        <Field label="Количество исполнителей">
                            <select value={workers} onChange={(e) => setWorkers(Number(e.target.value))}>
                                <option value={1}>1 узел</option><option value={2}>2 узла</option><option value={3}>3 узла</option>
                            </select>
                        </Field>
                        <Field label="Политика диспетчеризации">
                            <select value={policy} onChange={(e) => setPolicy(e.target.value)}>
                                <option value="predicted">По прогнозируемому времени</option>
                                <option value="least_loaded">По текущей загрузке</option>
                                <option value="round_robin">Циклический обход</option>
                            </select>
                        </Field>
                    </div>
                    <div className="experiment-actions">
                        <button className="button button-primary" onClick={configure} disabled={busy}>Применить конфигурацию</button>
                        <button className="button button-quiet" onClick={() => action('/api/reset')} disabled={busy}>Сбросить</button>
                        <span className="action-separator" />
                        <button className="button button-run" onClick={() => action('/api/start')} disabled={busy}>▶ Запустить</button>
                        <button className="button button-secondary" onClick={() => action('/api/pause')} disabled={busy}>Пауза</button>
                        <button className="button button-secondary" onClick={() => action('/api/resume')} disabled={busy}>Продолжить</button>
                    </div>
                    <div className="info-strip">
                        <strong>Воспроизводимость</strong>
                        <span>При одинаковых параметрах платформа использует одинаковые исходные матрицы, что позволяет сопоставлять результаты разных политик диспетчеризации.</span>
                    </div>
                </section>
            )}

            {page === 'analytics' && (
                <>
                    <section className="kpi-grid">
                        <Metric
                            label="ВЫЧИСЛЕНО БЛОКОВ"
                            value={String(s.numerical_validation?.computed_tiles ?? 0)}
                            detail={`Из ${s.numerical_validation?.total_tiles ?? s.total_tasks}`}
                        />
                        <Metric
                            label="ОТНОСИТЕЛЬНАЯ ОШИБКА"
                            value={s.numerical_validation?.relative_frobenius_error == null ? '—' : s.numerical_validation.relative_frobenius_error.toExponential(3)}
                            detail="Норма Фробениуса"
                        />
                        <Metric
                            label="ПРОВЕРКА РЕЗУЛЬТАТА"
                            value={s.numerical_validation?.done ? (s.numerical_validation.passed ? 'Пройдена' : 'Ошибка') : 'Ожидание'}
                            detail="Сравнение с эталонной матрицей"
                            accent={s.numerical_validation?.done ? (s.numerical_validation.passed ? 'green' : 'red') : 'neutral'}
                        />
                        <Metric
                            label="ВЫЧИСЛИТЕЛЬНАЯ ЗАГРУЗКА"
                            value={`${(s.worker_utilization * 100).toFixed(1)}%`}
                            detail="Доля вычислительного времени"
                            accent="blue"
                        />
                    </section>

                    <section className="surface page-surface">
                        <PanelTitle
                            eyebrow="БАЛАНСИРОВКА НАГРУЗКИ"
                            title="Распределение вычислений"
                            description="Накопленное время вычислений и число завершённых блоков по каждому узлу"
                        />
                        <div className="load-list">
                            {s.workers.map((w) => {
                                const maxBusy = Math.max(...s.workers.map((x) => x.busy_us), 1);
                                return (
                                    <div className="load-row" key={w.id}>
                                        <div className="load-node">N{w.id + 1}</div>
                                        <div className="load-main">
                                            <div className="load-row-label">
                                                <strong>Узел {w.id + 1}</strong>
                                                <span>{fmt(w.busy_us)} · {w.completed_tiles} блоков</span>
                                            </div>
                                            <div className="progress-track"><div className="progress-fill" style={{ width: `${(w.busy_us / maxBusy) * 100}%` }} /></div>
                                        </div>
                                    </div>
                                );
                            })}
                            {s.workers.length === 0 && <div className="empty-state compact">Данные появятся после получения состояния системы.</div>}
                        </div>
                    </section>

                    <section className="surface page-surface validation-panel">
                        <PanelTitle eyebrow="ЧИСЛЕННАЯ КОРРЕКТНОСТЬ" title="Проверка результата" />
                        <div className={`validation-result ${s.numerical_validation?.done ? (s.numerical_validation.passed ? 'passed' : 'failed') : ''}`}>
                            <span className="validation-mark">
                                {s.numerical_validation?.done ? (s.numerical_validation.passed ? '✓' : '!') : '…'}
                            </span>
                            <div>
                                <strong>
                                    {s.numerical_validation?.done
                                        ? (s.numerical_validation.passed ? 'Результат соответствует эталону' : 'Обнаружено расхождение с эталоном')
                                        : 'Проверка ожидает завершения вычислений'}
                                </strong>
                                <div className="formula">
                                    <span className="formula-variable">ε<sub>F</sub></span>

                                    <span className="formula-equals">=</span>

                                    <span className="fraction">
        <span className="fraction-top">
            ‖C − C<sub>ref</sub>‖<sub>F</sub>
        </span>
        <span className="fraction-bottom">
            ‖C<sub>ref</sub>‖<sub>F</sub>
        </span>
    </span>
                                </div>
                            </div>
                        </div>
                    </section>

                    <section className="surface page-surface">
                        <PanelTitle
                            eyebrow="ИСТОРИЯ ЗАПУСКОВ"
                            title="Последние три вычисления"
                            description="Завершённые запуски сохраняются при сбросе текущего эксперимента"
                            aside={<span className="history-count">{latestRuns.length} / 3</span>}
                        />
                        {latestRuns.length === 0 ? (
                            <div className="empty-state">Завершённых вычислений пока нет. После первого запуска здесь появится его сводка.</div>
                        ) : (
                            <div className="table-container">
                                <table className="history-table">
                                    <thead>
                                    <tr>
                                        <th>ЗАПУСК</th><th>МАТРИЦА</th><th>БЛОК</th><th>УЗЛЫ</th>
                                        <th>РАСПРЕДЕЛЕНИЕ</th><th>ПОЛИТИКА</th><th>ВРЕМЯ</th><th>ЗАГРУЗКА</th><th>ОШИБКА</th><th>ПРОВЕРКА</th>
                                    </tr>
                                    </thead>
                                    <tbody>
                                    {latestRuns.map((run) => (
                                        <tr key={run.run_id}>
                                            <td className="mono">#{run.run_id}</td>
                                            <td>{run.matrix_size}²</td>
                                            <td>{run.tile_size}²</td>
                                            <td>{run.worker_count}</td>
                                            <td className="distribution-cell">{run.workers.map((w) => `N${w.worker_id + 1}: ${w.completed_tiles}`).join(' · ')}</td>
                                            <td>{policyName(run.policy)}</td>
                                            <td>{fmt(run.sim_time_us)}</td>
                                            <td>{(run.worker_utilization * 100).toFixed(1)}%</td>
                                            <td>{run.relative_frobenius_error.toExponential(2)}</td>
                                            <td><span className={`result-tag ${run.passed ? 'ok' : 'bad'}`}>{run.passed ? 'Пройдена' : 'Ошибка'}</span></td>
                                        </tr>
                                    ))}
                                    </tbody>
                                </table>
                            </div>
                        )}
                    </section>
                </>
            )}

            <footer className="app-footer">
                <span>HETEROCOMPUTE · ПРОГРАММНАЯ ПЛАТФОРМА</span>
                <span>Блочные матричные вычисления · v0.4</span>
            </footer>
        </main>
    );
}

function PanelTitle({
                        eyebrow,
                        title,
                        description,
                        aside,
                    }: {
    eyebrow: string;
    title: string;
    description?: string;
    aside?: ReactNode;
}) {
    return (
        <div className="panel-title">
            <div>
                <div className="eyebrow">{eyebrow}</div>
                <h2>{title}</h2>
                {description && <p>{description}</p>}
            </div>
            {aside && <div className="panel-aside">{aside}</div>}
        </div>
    );
}

function Metric({
                    label,
                    value,
                    detail,
                    accent = 'neutral',
                    progress,
                }: {
    label: string;
    value: string;
    detail?: string;
    accent?: string;
    progress?: number;
}) {
    return (
        <article className={`metric-card accent-${accent}`}>
            <div className="metric-label">{label}</div>
            <div className="metric-value">{value}</div>
            {progress !== undefined && (
                <div className="metric-progress"><span style={{ width: `${progress}%` }} /></div>
            )}
            {detail && <div className="metric-detail">{detail}</div>}
        </article>
    );
}

function Field({ label, children }: { label: string; children: ReactNode }) {
    return <label className="field"><span>{label}</span>{children}</label>;
}

function StatePill({ state }: { state: string }) {
    return <span className={`state-pill state-${state.toLowerCase()}`}>{workerStateName(state)}</span>;
}

function TaskPill({ state }: { state: string }) {
    return <span className={`task-pill task-${state}`}>{taskStateName(state)}</span>;
}

function DataPoint({ label, value }: { label: string; value: string }) {
    return <div className="data-point"><span>{label}</span><strong>{value}</strong></div>;
}

function EventTable({ events, fmt }: { events: Event[]; fmt: (value: number) => string }) {
    return (
        <div className="table-container event-table-container">
            <table>
                <thead>
                <tr><th>ВРЕМЯ</th><th>СОБЫТИЕ</th><th>ЗАДАЧА</th><th>УЗЕЛ</th></tr>
                </thead>
                <tbody>
                {[...events].reverse().map((event, index) => (
                    <tr key={`${event.time_us}-${event.type}-${index}`}>
                        <td className="mono">{fmt(event.time_us)}</td>
                        <td><span className="event-name">{eventName(event.type)}</span></td>
                        <td className="mono">{event.task_id >= 0 ? `#${event.task_id}` : '—'}</td>
                        <td className="mono">{event.worker_id >= 0 ? `N${event.worker_id + 1}` : '—'}</td>
                    </tr>
                ))}
                {events.length === 0 && (
                    <tr><td colSpan={4} className="table-empty">События появятся после запуска вычислительного задания.</td></tr>
                )}
                </tbody>
            </table>
        </div>
    );
}

function policyName(policy: string) {
    return ({
        predicted: 'По прогнозу',
        least_loaded: 'Наименее загруженный',
        round_robin: 'Циклический',
    } as Record<string, string>)[policy] ?? policy;
}

function workerStateName(state: string) {
    return ({
        IDLE: 'ОЖИДАНИЕ',
        BUSY: 'ВЫЧИСЛЕНИЕ',
        WAIT_RESULT: 'ОЖИДАНИЕ РЕЗУЛЬТАТА',
        TRANSFER: 'ПЕРЕДАЧА',
        FAILED: 'НЕДОСТУПЕН',
        RECOVERY: 'ВОССТАНОВЛЕНИЕ',
    } as Record<string, string>)[state] ?? state;
}

function taskStateName(state: string) {
    return ({
        queued: 'В ОЧЕРЕДИ',
        dispatched: 'НАЗНАЧЕНА',
        computing: 'ВЫЧИСЛЯЕТСЯ',
        ready: 'РЕЗУЛЬТАТ ГОТОВ',
        completed: 'ЗАВЕРШЕНА',
    } as Record<string, string>)[state] ?? state;
}

function jobStateName(state: string) {
    return ({
        idle: 'ОЖИДАНИЕ',
        running: 'ВЫПОЛНЯЕТСЯ',
        paused: 'ПРИОСТАНОВЛЕНО',
        completed: 'ЗАВЕРШЕНО',
        failed: 'ОШИБКА',
    } as Record<string, string>)[state] ?? state.toUpperCase();
}

function eventName(event: string) {
    return ({
        TASK_CREATED: 'Задача создана',
        SPI_INPUT_TRANSFER_STARTED: 'Начата передача входных данных',
        SPI_INPUT_TRANSFER_COMPLETED: 'Передача входных данных завершена',
        COMPUTATION_STARTED: 'Начато вычисление блока',
        COMPUTATION_COMPLETED: 'Вычисление блока завершено',
        READY_ASSERTED: 'Активирован сигнал READY#',
        SPI_RESULT_TRANSFER_STARTED: 'Начата передача результата',
        RESULT_VALIDATED: 'Результат принят ведущим узлом',
        ACK_SENT: 'Отправлено подтверждение ACK',
        READY_DEASSERTED: 'Сигнал READY# сброшен',
        WORKER_IDLE: 'Исполнитель готов к следующей задаче',
        MATRIX_VALIDATION_PASSED: 'Численная проверка пройдена',
        MATRIX_VALIDATION_FAILED: 'Ошибка численной проверки',
    } as Record<string, string>)[event] ?? event.replaceAll('_', ' ').toLowerCase();
}
