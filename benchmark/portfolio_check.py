#!/usr/bin/env python3
"""Validate the improved executable against disposable API/SQL fixtures."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--app', required=True, type=Path)
parser.add_argument('--binary', required=True, type=Path)
parser.add_argument('--prepared', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
app, binary = args.app.resolve(), args.binary.resolve()
prepared = json.loads(args.prepared.read_text())
suffix = str(time.time_ns())
prefix = 'forge-improvement-' + suffix
network, db, checker = prefix + '-net', prefix + '-db', prefix + '-checker'
image = 'forge-portfolio-improved:' + suffix
created, network_created = [], False


def docker(*values, input=None):
    stderr = subprocess.STDOUT if values and values[0] == 'logs' else None
    return subprocess.check_output(['docker', *map(str, values)], input=input, text=True,
                                   stderr=stderr).strip()


def sql(database, command):
    return docker('exec', '-i', db, 'psql', '-U', 'forge', '-d', database,
                  '-v', 'ON_ERROR_STOP=1', '-At', input=command)


def api(name, source_image, database, pool=5):
    docker('create', '--name', name, '--label', 'forge.improvement=20261003',
           '--network', network, '--network-alias', 'api', '--cpus', '2', '--memory', '512m',
           '-e', f'DATABASE_URL=postgres://forge:forge-test-password@{db}/{database}',
           '-e', f'DATABASE_POOL_SIZE={pool}', '-e', 'JWT_SECRET=forge-integration-test-secret',
           '-e', 'GITHUB_CLIENT_ID=test-client', '-e', 'GITHUB_CLIENT_SECRET=test-client-secret',
           '-e', 'ADMIN_GITHUB_USERNAME=Helloworld0822', '-e', 'FRONTEND_URL=http://frontend.test',
           '-e', 'BACKEND_BASE_URL=http://api:8080', '-e', 'CORS_ALLOWED_ORIGINS=http://frontend.test',
           '-e', 'GITHUB_OAUTH_BASE_URL=http://checker:19090',
           '-e', 'GITHUB_API_BASE_URL=http://checker:19090',
           '-e', f'FORGE_TRUSTED_PROXIES={subnet}', source_image)
    created.append(name)
    docker('start', name)
    for _ in range(60):
        probe = subprocess.run(['docker', 'exec', name, 'curl', '-fsS',
                                'http://127.0.0.1:8080/api/health'], capture_output=True)
        if probe.returncode == 0:
            return
        time.sleep(.2)
    raise RuntimeError('API failed to start: ' + docker('logs', name))


try:
    for key in ['before', 'after']:
        if docker('image', 'inspect', '-f', '{{.Id}}', prepared['images'][key]) != prepared['image_ids'][key]:
            raise RuntimeError('Prepared image changed')
    if subprocess.run(['docker', 'image', 'inspect', image], capture_output=True).returncode == 0:
        raise RuntimeError('Refusing existing image')
    with tempfile.TemporaryDirectory(prefix='forge-improvement-image-') as directory:
        context = Path(directory)
        shutil.copy2(binary, context / 'portfolio-api')
        shutil.copyfile(app / 'backend/migrations/0011_restore_comment_post_index.sql', context / '0011.sql')
        (context / 'Dockerfile').write_text(f"FROM {prepared['images']['after']}\n"
            'COPY portfolio-api /app/portfolio-api\n'
            'COPY 0011.sql /app/backend/migrations/0011_restore_comment_post_index.sql\n')
        subprocess.run(['docker', 'build', '-t', image, str(context)], check=True)
    docker('network', 'create', '--label', 'forge.improvement=20261003', network)
    network_created = True
    subnet = docker('network', 'inspect', '-f', '{{(index .IPAM.Config 0).Subnet}}', network)
    docker('create', '--name', db, '--label', 'forge.improvement=20261003', '--network', network,
           '--memory', '512m', '--tmpfs', '/var/lib/postgresql/data', '-e', 'POSTGRES_USER=forge',
           '-e', 'POSTGRES_PASSWORD=forge-test-password', '-e', 'POSTGRES_DB=forge_test', 'postgres:16-alpine')
    created.append(db)
    docker('start', db)
    for _ in range(60):
        if subprocess.run(['docker', 'exec', db, 'pg_isready', '-U', 'forge'], capture_output=True).returncode == 0:
            break
        time.sleep(.2)
    else:
        raise RuntimeError('DB did not start')
    api_name = prefix + '-api'
    api(api_name, image, 'forge_test', 1)
    if 'threads=connection' not in docker('logs', api_name):
        raise RuntimeError('Improved default mode not active')
    command = ['docker', 'run', '--rm', '--name', checker, '--label', 'forge.improvement=20261003',
        '--network', network, '--network-alias', 'checker', '--entrypoint', 'sh',
        '-v', f'{app}:/app:ro', '-e', 'API_BASE=http://api:8080',
        '-e', f'TEST_DATABASE_URL=postgres://forge:forge-test-password@{db}/forge_test',
        'postgres:16-alpine', '-ec',
        'apk add --no-cache python3 >/dev/null; python3 /app/backend-forge/tests/integration.py']
    created.append(checker)
    subprocess.run(command, check=True)
    for _ in range(2):
        assert sql('forge_test', 'SELECT count(*) FROM _sqlx_migrations WHERE success;') == '11'
        assert sql('forge_test', "SELECT indisvalid AND indisready FROM pg_index WHERE indexrelid='comments_post_id_created_at_idx'::regclass AND indrelid='comments'::regclass;") == 't'
        if _ == 0:
            docker('restart', api_name)
            for attempt in range(60):
                if subprocess.run(['docker', 'exec', api_name, 'curl', '-fsS',
                        'http://127.0.0.1:8080/api/health'], capture_output=True).returncode == 0:
                    break
                time.sleep(.2)
            else:
                raise RuntimeError('API restart failed')
    docker('stop', '-t', '10', api_name)
    assert docker('inspect', '-f', '{{.State.ExitCode}}', api_name) == '0'
    docker('exec', db, 'createdb', '-U', 'forge', 'sql_test')
    sql_api = prefix + '-sql-api'
    api(sql_api, prepared['images']['after'], 'sql_test')
    docker('stop', '-t', '10', sql_api)
    docker('rm', sql_api)
    assert sql('sql_test', "SELECT to_regclass('comments_post_id_created_at_idx') IS NULL;") == 't'
    sql('sql_test', """TRUNCATE comments, posts RESTART IDENTITY CASCADE;
    INSERT INTO posts(title,content_markdown,excerpt,published,created_at,updated_at)
    SELECT 'Post '||i,'body','excerpt',i<=20,'2026-01-01'::timestamptz,'2026-01-01'::timestamptz
    FROM generate_series(1,1000)i;
    INSERT INTO comments(post_id,author_login,body,created_at)
    SELECT ((i-1)%1000)+1,'fixture-user','comment', '2026-01-01'::timestamptz
    FROM generate_series(1,200000)i;
    ANALYZE posts; ANALYZE comments;""")
    rows_query = ('SELECT p.id,p.title,p.excerpt,p.created_at,COUNT(c.id) AS comment_count '
        'FROM posts p LEFT JOIN comments c ON c.post_id=p.id WHERE p.published=true '
        'GROUP BY p.id ORDER BY p.created_at DESC')
    json_query = "SELECT COALESCE(json_agg(item),'[]'::json) FROM (" + rows_query + ') item'
    gate = ("WITH gate AS MATERIALIZED (SELECT EXISTS(SELECT 1 FROM banned_ips WHERE ip='8.8.8.12' "
        "AND (expires_at IS NULL OR expires_at>now())) AS banned) SELECT banned,CASE WHEN banned "
        "THEN NULL::json ELSE (" + json_query + ') END FROM gate')
    queries = {'rows': rows_query, 'json': json_query, 'guarded_json': gate,
        'comments_for_post': 'SELECT id,author_login,body,created_at FROM comments WHERE post_id=1 ORDER BY created_at DESC',
        'ban_only': "SELECT EXISTS(SELECT 1 FROM banned_ips WHERE ip='8.8.8.12' AND (expires_at IS NULL OR expires_at>now()))"}
    fingerprint = sql('sql_test', 'SELECT count(*),sum(post_id) FROM comments;')
    result = {'image': image, 'image_id': docker('image', 'inspect', '-f', '{{.Id}}', image),
        'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
        'integration': 'passed with pool1 and mock GitHub; default connection mode; migration11 restart',
        'fixture': {'posts': 1000, 'published_posts': 20, 'comments': 200000},
        'queries': queries, 'plans': {}}
    for phase in ['missing_index', 'restored_index']:
        if phase == 'restored_index':
            api(sql_api, image, 'sql_test')
            docker('stop', '-t', '10', sql_api)
            sql('sql_test', (app / 'backend/migrations/0011_restore_comment_post_index.sql').read_text())
            assert sql('sql_test', 'SELECT count(*) FROM _sqlx_migrations WHERE success;') == '11'
            assert sql('sql_test', 'SELECT count(*),sum(post_id) FROM comments;') == fingerprint
        phase_plans = {}
        for key, query in queries.items():
            plans = [json.loads(sql('sql_test', 'EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, FORMAT JSON) ' + query))[0]
                     for _ in range(7)]
            phase_plans[key] = plans
        result['plans'][phase] = phase_plans
    result['index_definition'] = sql('sql_test', "SELECT pg_get_indexdef('comments_post_id_created_at_idx'::regclass);")
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'image': image, 'output': str(args.output), 'integration': 'passed', 'sql': 'passed'}))
finally:
    for name in reversed(created):
        subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if network_created:
        subprocess.run(['docker', 'network', 'rm', network], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
