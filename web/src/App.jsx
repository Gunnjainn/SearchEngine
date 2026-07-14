import { useState } from 'react'
import './App.css'

const API_URL = import.meta.env.VITE_API_URL || 'http://localhost:8000'

function App() {
  const [query, setQuery] = useState('')
  const [results, setResults] = useState(null)
  const [latencyMs, setLatencyMs] = useState(null)
  const [loading, setLoading] = useState(false)
  const [error, setError] = useState(null)

  const handleSearch = async (e) => {
    e.preventDefault()
    const trimmed = query.trim()
    if (!trimmed) return

    setLoading(true)
    setError(null)
    setResults(null)
    setLatencyMs(null)

    try {
      const resp = await fetch(`${API_URL}/search`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ query: trimmed, k: 10 }),
      })

      if (!resp.ok) {
        throw new Error(`Gateway returned ${resp.status}`)
      }

      const data = await resp.json()
      setResults(data.results)
      setLatencyMs(data.latency_ms)
    } catch (err) {
      setError(err.message || 'Something went wrong')
    } finally {
      setLoading(false)
    }
  }

  const hasResults = results !== null

  return (
    <div className="app">
      {/* ── Header ─────────────────────────────────────────── */}
      <header className={`header ${hasResults ? 'header--compact' : ''}`}>
        <h1 className="logo">SearchEngine</h1>
        <p className="tagline">Hand-rolled BM25 · Built from scratch</p>
      </header>

      {/* ── Search Box ─────────────────────────────────────── */}
      <div className="search-container">
        <form className="search-form" onSubmit={handleSearch} id="search-form">
          <input
            id="search-input"
            className="search-input"
            type="text"
            placeholder="Search for anything…"
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            autoFocus
          />
          <button
            id="search-button"
            className="search-button"
            type="submit"
            aria-label="Search"
          >
            ⌕
          </button>
        </form>
      </div>

      {/* ── Results Area ───────────────────────────────────── */}
      <div className="results-area">
        {loading && (
          <div className="loading-indicator">
            <div className="spinner" />
            Searching…
          </div>
        )}

        {error && (
          <div className="error-message" id="error-message">
            {error}
          </div>
        )}

        {hasResults && !loading && (
          <>
            <div className="results-meta">
              <span className="results-count" id="results-count">
                {results.length} result{results.length !== 1 ? 's' : ''}
              </span>
              {latencyMs !== null && (
                <span className="results-latency" id="results-latency">
                  {latencyMs} ms
                </span>
              )}
            </div>

            {results.length === 0 ? (
              <div className="empty-state">No results found.</div>
            ) : (
              <div className="results-list" id="results-list">
                {results.map((r, i) => (
                  <article className="result-card" key={`${r.doc_id}-${i}`}>
                    <div className="result-header">
                      <span className="result-doc-id">doc #{r.doc_id}</span>
                      <span className="result-score">
                        score {r.score.toFixed(2)}
                      </span>
                    </div>
                    <p className="result-snippet">{r.snippet}</p>
                  </article>
                ))}
              </div>
            )}
          </>
        )}
      </div>
    </div>
  )
}

export default App
