# ---------------------------------------------------------------------------
# Search engine — one-command operations.
#
#   make run     bring the whole stack up and wait until it is healthy
#   make build   build every image
#   make corpus  fetch the Hacker News corpus into ingest/data/
#   make index   build the on-disk index from the corpus as a discrete step
#   make test    run every test suite
#
# `make run` is enough on a clean checkout: it fetches the corpus if missing,
# and the engine builds its index on first boot.
#
# Every recipe is a single docker/compose command so it behaves the same under
# sh, Git Bash and cmd.exe. On Windows without `make`, either use
# `mingw32-make <target>` or run the command shown under each target directly —
# README.md lists them all.
# ---------------------------------------------------------------------------

COMPOSE     := docker compose -f infra/docker-compose.yml
CORPUS      := ingest/data/corpus.sample.jsonl
API_PORT    ?= 8000
ENGINE_PORT ?= 8080
WEB_PORT    ?= 5173

.DEFAULT_GOAL := help
.PHONY: help build corpus index reindex run up down restart logs ps smoke stats bench \
        test test-engine test-api test-ingest test-web clean

## help: list the available targets
help:
	@echo "Targets:"
	@echo "  make run       bring the stack up and wait until it is healthy"
	@echo "  make build     build every image"
	@echo "  make corpus    fetch the HN corpus into $(CORPUS)"
	@echo "  make index     build the on-disk index from the corpus"
	@echo "  make reindex   discard the index and build it again"
	@echo "  make smoke     POST a real query and print the response"
	@echo "  make stats     index and query-cache counters"
	@echo "  make bench     query latency p50/p95, cache off vs warm"
	@echo "  make test      run every test suite"
	@echo "  make ps        show service status and health"
	@echo "  make logs      follow logs from every service"
	@echo "  make down      stop the stack (keeps index and database)"
	@echo "  make clean     stop the stack and delete its volumes"
	@echo ""
	@echo "URLs once running:  web    http://localhost:$(WEB_PORT)"
	@echo "                    api    http://localhost:$(API_PORT)/docs"
	@echo "                    engine http://localhost:$(ENGINE_PORT)/health"

## build: build every image. The engine image runs all 8 C++ suites as it builds.
build:
	$(COMPOSE) build

## corpus: fetch the corpus in a container, so no host Python is needed
corpus:
	$(COMPOSE) run --rm ingest

# Fetch the corpus only when it is missing.
$(CORPUS):
	$(MAKE) corpus

## index: build the on-disk index from the corpus, then exit
index: $(CORPUS)
	$(COMPOSE) run --rm -e BUILD_ONLY=1 engine

## reindex: throw the index volume away and build it again
reindex:
	-$(COMPOSE) rm -sf engine
	-docker volume rm infra_engine_index
	$(MAKE) index

## run: bring everything up and block until every service is healthy
run: $(CORPUS)
	$(COMPOSE) up -d --build --wait
	@echo ""
	@echo "Stack is up:"
	@echo "  web    http://localhost:$(WEB_PORT)"
	@echo "  api    http://localhost:$(API_PORT)/docs"
	@echo "  engine http://localhost:$(ENGINE_PORT)/health"
	@echo ""
	@echo "Try:  make smoke"

## up: same as run, without rebuilding images
up:
	$(COMPOSE) up -d --wait

## down: stop the stack, keeping the index and database volumes
down:
	$(COMPOSE) down

## restart: recreate the running services
restart:
	$(COMPOSE) up -d --force-recreate --wait

## logs: follow logs from every service
logs:
	$(COMPOSE) logs -f

## ps: show service status and health
ps:
	$(COMPOSE) ps

## stats: index and query-cache counters from the engine
stats:
	curl -s http://localhost:$(ENGINE_PORT)/stats

## bench: query latency p50/p95, cache disabled vs warm, against the frozen corpus
bench:
	$(COMPOSE) run --rm --no-deps -v "$(CURDIR)/eval:/eval:ro" --entrypoint bench_query engine /eval/corpus.frozen.jsonl

## smoke: send a real query through the API gateway and print the response
smoke:
	curl -s -X POST http://localhost:$(API_PORT)/search -H "Content-Type: application/json" -d "{\"query\":\"python compiler\",\"k\":3}"

## test: run every suite
test: test-engine test-api test-ingest test-web

## test-engine: the 8 C++ suites — ctest runs inside the image build
test-engine:
	docker build -f engine/Dockerfile engine

## test-api: FastAPI gateway tests
test-api:
	$(COMPOSE) run --rm --no-deps api python -m pytest tests -q

## test-ingest: ingestion tests, all HTTP mocked
test-ingest:
	$(COMPOSE) run --rm --no-deps --entrypoint python ingest -m pytest test_hn_fetch.py -q

## test-web: frontend lint
test-web:
	$(COMPOSE) run --rm --no-deps web npm run lint

## clean: stop the stack and delete the index and database volumes
clean:
	$(COMPOSE) down -v
