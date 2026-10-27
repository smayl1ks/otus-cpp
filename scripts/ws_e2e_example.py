import asyncio, json, time, urllib.request, sys
sys.path.insert(0, '/tmp/pbgen')
import websockets
import envelope_pb2, session_pb2, telemetry_pb2, common_pb2

BASE = "http://localhost:8081"

def http_login():
    req = urllib.request.Request(BASE + "/api/v1/auth/login",
        data=json.dumps({"login": "pguser", "password": "pguser_secret"}).encode(),
        headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(req).read())

def make_point(lat, lon, ts):
    p = telemetry_pb2.LocationPoint()
    p.latitude, p.longitude, p.timestamp, p.accuracy = lat, lon, ts, 5.0
    return p

def make_batch(sid, uuid, seq, base_ts):
    b = telemetry_pb2.LocationBatch()
    b.session_id, b.batch_uuid, b.sequence_number = sid, uuid, seq
    b.state = telemetry_pb2.RUNNING_STATE_ACTIVE
    for i in range(4):
        pt = b.points.add()
        pt.latitude = 55.79830 + 0.000611 * i
        pt.longitude = 49.10610 + 0.000360 * i
        pt.accuracy = 5.0
        pt.timestamp = base_ts + 20_000 * i
    return b

def wrap(payload_field, msg):
    e = envelope_pb2.Envelope()
    e.message_id = f"e2e-{time.time_ns()}"
    e.protocol_version = 1
    getattr(e, payload_field).CopyFrom(msg)
    return e.SerializeToString()

async def main():
    auth = http_login()
    ticket = auth["ws_ticket"]
    session_id = f"e2e-ws-run-{int(time.time())}"
    async with websockets.connect(f"ws://localhost:8081/ws?token={ticket}",
                                  max_size=2**22) as ws:
        # START
        start = session_pb2.StartRunRequest()
        start.session_id = session_id
        start.team_id = 1
        start.visibility = session_pb2.VISIBILITY_PUBLIC
        start.mode = session_pb2.RUN_MODE_ONLINE
        start.start_timestamp = int(time.time() * 1000)
        await ws.send(wrap("start_run_request", start))

        resp = envelope_pb2.Envelope()
        resp.ParseFromString(await asyncio.wait_for(ws.recv(), 5))
        sr = resp.start_run_response
        assert sr.system_status == common_pb2.SYSTEM_STATUS_OK, sr
        assert not sr.resumed
        run_id = sr.server_run_id
        print(f"START ok: server_run_id={run_id}, max_points_per_batch={sr.max_points_per_batch}")

        # BATCHES
        base_ts = int(time.time() * 1000) - 150_000
        for seq in (1, 2):
            await ws.send(wrap("location_batch", make_batch(session_id, f"batch-{seq}", seq, base_ts + 80_000 * (seq - 1))))
            resp.ParseFromString(await asyncio.wait_for(ws.recv(), 5))
            ack = resp.batch_process_response
            assert ack.anticheat_status == telemetry_pb2.ANTICHEAT_STATUS_OK, ack
            print(f"BATCH {seq} ok: accepted={ack.accepted_points_count}, dist={ack.total_distance_meters:.1f} m")

        # FINISH
        finish = session_pb2.FinishRunRequest()
        finish.session_id = session_id
        finish.finish_timestamp = int(time.time() * 1000)
        await ws.send(wrap("finish_run_request", finish))
        resp.ParseFromString(await asyncio.wait_for(ws.recv(), 5))
        fr = resp.finish_run_response
        assert fr.system_status == common_pb2.SYSTEM_STATUS_OK, fr
        s = fr.summary
        print(f"FINISH ok: {s.total_distance_meters:.1f} m, {s.duration_seconds} s, "
              f"hexes={s.hexagons_captured_count}, score={s.total_score}, valid={s.is_valid}")
        assert s.total_points_count == 8
        assert s.server_run_id == run_id
        print("WS E2E PASS")

asyncio.run(main())
