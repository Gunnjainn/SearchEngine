# Web Frontend

React + Vite single-page application providing the search UI.

## Development

```bash
npm install
npm run dev
```

Open http://localhost:5173.

## Environment

| Variable       | Default                | Description       |
|----------------|------------------------|-------------------|
| `VITE_API_URL` | `http://localhost:8000` | Gateway base URL  |

## Docker

```bash
docker compose -f infra/docker-compose.yml up web
```
