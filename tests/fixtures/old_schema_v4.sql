PRAGMA user_version = 4;
CREATE TABLE downloads (
  id INTEGER PRIMARY KEY,
  url TEXT NOT NULL,
  dest_path TEXT NOT NULL,
  total_size INTEGER DEFAULT 0,
  status TEXT DEFAULT 'QUEUED',
  priority INTEGER DEFAULT 0,
  created_at INTEGER,
  cookie TEXT DEFAULT '',
  referrer TEXT DEFAULT '',
  extra_headers TEXT DEFAULT '',
  expected_sha256 TEXT DEFAULT '',
  speed_limit_bps INTEGER DEFAULT 0,
  reserved_file INTEGER DEFAULT 0,
  etag TEXT DEFAULT '',
  last_modified TEXT DEFAULT '',
  auto_filename INTEGER DEFAULT 0,
  auth_user TEXT DEFAULT '',
  auth_password TEXT DEFAULT ''
);
CREATE TABLE chunks (
  download_id INTEGER,
  range_start INTEGER,
  range_end INTEGER,
  bytes_done INTEGER DEFAULT 0,
  FOREIGN KEY(download_id) REFERENCES downloads(id) ON DELETE CASCADE
);
INSERT INTO downloads (id, url, dest_path, status)
VALUES (77, 'http://127.0.0.1/old', '/tmp/cdm-v4-old', 'PAUSED');
INSERT INTO chunks (download_id, range_start, range_end, bytes_done)
VALUES (77, 0, 99, 40);
