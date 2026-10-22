"""Small persistent local community profile store for the WON emulator."""

import hashlib
import hmac
import secrets
import sqlite3
import threading
import time
from contextlib import contextmanager
from pathlib import Path

LOCAL_PASSWORD_ITERATIONS = 310_000
DEFAULT_PROFILE_GRAPHIC = "texticons/twb/twb_Laserrifle.jpg"


class CommunityStore:
    PROFILE_FIELDS = frozenset({"display_name", "tag", "appending", "description", "url",
                                "graphic", "primary_tribe"})
    TRIBE_FIELDS = frozenset({"tag", "appending", "recruiting", "description", "graphic"})

    def __init__(self, path):
        self.path = Path(path).expanduser().resolve()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.parent.chmod(0o700)
        self.lock = threading.RLock()
        self._initialize()

    def _connect(self):
        connection = sqlite3.connect(self.path, timeout=5)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        return connection

    @contextmanager
    def _connection(self):
        connection = self._connect()
        try:
            yield connection
            connection.commit()
        except Exception:
            connection.rollback()
            raise
        finally:
            connection.close()

    def _initialize(self):
        with self.lock, self._connection() as connection:
            connection.execute("PRAGMA journal_mode = WAL")
            connection.execute("""
                CREATE TABLE IF NOT EXISTS profiles (
                    user_id INTEGER PRIMARY KEY,
                    username TEXT NOT NULL UNIQUE COLLATE NOCASE,
                    display_name TEXT NOT NULL,
                    tag TEXT NOT NULL DEFAULT '',
                    appending INTEGER NOT NULL DEFAULT 0,
                    description TEXT NOT NULL DEFAULT '',
                    url TEXT NOT NULL DEFAULT '',
                    graphic TEXT NOT NULL DEFAULT 'texticons/twb/twb_Laserrifle.jpg',
                    primary_tribe TEXT NOT NULL DEFAULT '',
                    registered_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL
                )
            """)
            connection.execute("""
                CREATE TABLE IF NOT EXISTS auth_accounts (
                    username TEXT PRIMARY KEY COLLATE NOCASE,
                    salt BLOB,
                    password_hash BLOB,
                    iterations INTEGER NOT NULL DEFAULT 310000,
                    created_at INTEGER NOT NULL
                )
            """)
            # Stock Create/Edit Account fields. TribesNext stores neither, so
            # its accounts keep them here too (visible only on this machine).
            connection.execute("""
                CREATE TABLE IF NOT EXISTS account_info (
                    username TEXT PRIMARY KEY COLLATE NOCASE,
                    provider TEXT NOT NULL,
                    email TEXT NOT NULL DEFAULT '',
                    send_info INTEGER NOT NULL DEFAULT 0,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL
                )
            """)
            connection.execute("""
                CREATE TABLE IF NOT EXISTS tribes (
                    tribe_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    name TEXT NOT NULL UNIQUE COLLATE NOCASE,
                    tag TEXT NOT NULL,
                    appending INTEGER NOT NULL DEFAULT 0,
                    recruiting INTEGER NOT NULL DEFAULT 0,
                    description TEXT NOT NULL DEFAULT '',
                    graphic TEXT NOT NULL DEFAULT '',
                    created_at INTEGER NOT NULL
                )
            """)
            connection.execute("""
                CREATE TABLE IF NOT EXISTS memberships (
                    tribe_id INTEGER NOT NULL REFERENCES tribes(tribe_id) ON DELETE CASCADE,
                    user_id INTEGER NOT NULL REFERENCES profiles(user_id) ON DELETE CASCADE,
                    access INTEGER NOT NULL DEFAULT 0,
                    title TEXT NOT NULL DEFAULT '',
                    joined_at INTEGER NOT NULL,
                    PRIMARY KEY (tribe_id, user_id)
                )
            """)
            connection.execute("""
                CREATE TABLE IF NOT EXISTS invitations (
                    invite_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    tribe_id INTEGER NOT NULL REFERENCES tribes(tribe_id) ON DELETE CASCADE,
                    user_id INTEGER NOT NULL REFERENCES profiles(user_id) ON DELETE CASCADE,
                    inviter_id INTEGER REFERENCES profiles(user_id),
                    status TEXT NOT NULL DEFAULT 'pending',
                    created_at INTEGER NOT NULL,
                    UNIQUE (tribe_id, user_id)
                )
            """)
            invitation_columns = {row[1] for row in connection.execute(
                "PRAGMA table_info(invitations)")}
            if "inviter_id" not in invitation_columns:
                connection.execute(
                    "ALTER TABLE invitations ADD COLUMN inviter_id INTEGER REFERENCES profiles(user_id)")
            connection.execute("""
                CREATE TABLE IF NOT EXISTS mail (
                    mail_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    sender_id INTEGER NOT NULL REFERENCES profiles(user_id),
                    recipient_id INTEGER NOT NULL REFERENCES profiles(user_id),
                    is_cc INTEGER NOT NULL DEFAULT 0,
                    to_list TEXT NOT NULL DEFAULT '',
                    cc_list TEXT NOT NULL DEFAULT '',
                    subject TEXT NOT NULL,
                    body TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    is_read INTEGER NOT NULL DEFAULT 0,
                    is_deleted INTEGER NOT NULL DEFAULT 0
                )
            """)
            mail_columns = {row[1] for row in connection.execute("PRAGMA table_info(mail)")}
            if "to_list" not in mail_columns:
                connection.execute("ALTER TABLE mail ADD COLUMN to_list TEXT NOT NULL DEFAULT ''")
            if "cc_list" not in mail_columns:
                connection.execute("ALTER TABLE mail ADD COLUMN cc_list TEXT NOT NULL DEFAULT ''")
            connection.execute("""
                CREATE TABLE IF NOT EXISTS email_blocks (
                    owner_id INTEGER NOT NULL REFERENCES profiles(user_id) ON DELETE CASCADE,
                    blocked_id INTEGER NOT NULL REFERENCES profiles(user_id) ON DELETE CASCADE,
                    created_at INTEGER NOT NULL,
                    PRIMARY KEY (owner_id, blocked_id)
                )
            """)
            connection.execute("""
                CREATE TABLE IF NOT EXISTS buddies (
                    owner_id INTEGER NOT NULL REFERENCES profiles(user_id) ON DELETE CASCADE,
                    buddy_id INTEGER NOT NULL REFERENCES profiles(user_id) ON DELETE CASCADE,
                    created_at INTEGER NOT NULL,
                    PRIMARY KEY (owner_id, buddy_id)
                )
            """)
            # TribesNext has no player picture field; keep the chosen in-game
            # texture here by certificate GUID instead of seeding a local profile.
            connection.execute("""
                CREATE TABLE IF NOT EXISTS tribesnext_pictures (
                    guid INTEGER PRIMARY KEY,
                    graphic TEXT NOT NULL,
                    updated_at INTEGER NOT NULL
                )
            """)
            connection.execute("""
                CREATE TABLE IF NOT EXISTS forums (
                    forum_id INTEGER PRIMARY KEY,
                    name TEXT NOT NULL,
                    flags INTEGER NOT NULL DEFAULT 0,
                    tribe_id INTEGER NOT NULL DEFAULT 0
                )
            """)
            connection.execute("""
                INSERT OR IGNORE INTO forums (forum_id, name, flags, tribe_id)
                VALUES (1, 'General', 0, 0)
            """)
            connection.execute("""
                CREATE TABLE IF NOT EXISTS forum_topics (
                    topic_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    forum_id INTEGER NOT NULL REFERENCES forums(forum_id),
                    subject TEXT NOT NULL,
                    author_id INTEGER NOT NULL REFERENCES profiles(user_id),
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL,
                    is_deleted INTEGER NOT NULL DEFAULT 0,
                    locked INTEGER NOT NULL DEFAULT 0,
                    lock_reason TEXT NOT NULL DEFAULT '',
                    update_id INTEGER NOT NULL DEFAULT 0
                )
            """)
            connection.execute("""
                CREATE TABLE IF NOT EXISTS forum_update_sequence (
                    update_id INTEGER PRIMARY KEY AUTOINCREMENT
                )
            """)
            topic_columns = {row[1] for row in connection.execute(
                "PRAGMA table_info(forum_topics)")}
            if "locked" not in topic_columns:
                connection.execute(
                    "ALTER TABLE forum_topics ADD COLUMN locked INTEGER NOT NULL DEFAULT 0")
            if "lock_reason" not in topic_columns:
                connection.execute(
                    "ALTER TABLE forum_topics ADD COLUMN lock_reason TEXT NOT NULL DEFAULT ''")
            if "update_id" not in topic_columns:
                connection.execute(
                    "ALTER TABLE forum_topics ADD COLUMN update_id INTEGER NOT NULL DEFAULT 0")
            connection.execute("""
                CREATE TABLE IF NOT EXISTS forum_posts (
                    post_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    topic_id INTEGER NOT NULL REFERENCES forum_topics(topic_id),
                    parent_id INTEGER NOT NULL DEFAULT 0,
                    author_id INTEGER NOT NULL REFERENCES profiles(user_id),
                    subject TEXT NOT NULL,
                    body TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL,
                    is_deleted INTEGER NOT NULL DEFAULT 0,
                    update_id INTEGER NOT NULL DEFAULT 0
                )
            """)
            post_columns = {row[1] for row in connection.execute("PRAGMA table_info(forum_posts)")}
            if "update_id" not in post_columns:
                connection.execute(
                    "ALTER TABLE forum_posts ADD COLUMN update_id INTEGER NOT NULL DEFAULT 0")
            legacy_posts = connection.execute(
                "SELECT post_id, topic_id FROM forum_posts WHERE update_id = 0 ORDER BY post_id"
            ).fetchall()
            for post in legacy_posts:
                update_id = self._next_forum_update(connection)
                connection.execute("UPDATE forum_posts SET update_id = ? WHERE post_id = ?",
                                   (update_id, post["post_id"]))
                connection.execute("UPDATE forum_topics SET update_id = MAX(update_id, ?) "
                                   "WHERE topic_id = ?", (update_id, post["topic_id"]))
            connection.execute("""
                CREATE TABLE IF NOT EXISTS news_articles (
                    article_id INTEGER PRIMARY KEY AUTOINCREMENT,
                    category_id INTEGER NOT NULL,
                    author_id INTEGER NOT NULL REFERENCES profiles(user_id),
                    title TEXT NOT NULL,
                    body TEXT NOT NULL,
                    created_at INTEGER NOT NULL,
                    updated_at INTEGER NOT NULL,
                    is_deleted INTEGER NOT NULL DEFAULT 0
                )
            """)
            profile_columns = {row[1] for row in connection.execute("PRAGMA table_info(profiles)")}
            if "primary_tribe" not in profile_columns:
                connection.execute("ALTER TABLE profiles ADD COLUMN primary_tribe TEXT NOT NULL DEFAULT ''")
            connection.execute("""
                UPDATE profiles
                SET primary_tribe = (
                    SELECT t.name FROM memberships AS m
                    JOIN tribes AS t USING (tribe_id)
                    WHERE m.user_id = profiles.user_id
                    ORDER BY m.access DESC, m.joined_at, t.name
                    LIMIT 1
                )
                WHERE primary_tribe = '' AND EXISTS (
                    SELECT 1 FROM memberships WHERE user_id = profiles.user_id
                )
            """)
            connection.execute("UPDATE profiles SET graphic = ? WHERE graphic = ''",
                               (DEFAULT_PROFILE_GRAPHIC,))
        self.path.chmod(0o600)

    @staticmethod
    def _next_forum_update(connection):
        cursor = connection.execute("INSERT INTO forum_update_sequence DEFAULT VALUES")
        return int(cursor.lastrowid)

    def ensure_profile(self, username, user_id):
        username = str(username).strip()
        if not username:
            raise ValueError("Profile username cannot be empty")
        now = int(time.time())
        with self.lock, self._connection() as connection:
            connection.execute("""
                INSERT INTO profiles (user_id, username, display_name, graphic,
                                      registered_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(username) DO NOTHING
            """, (int(user_id), username, username, DEFAULT_PROFILE_GRAPHIC, now, now))
            row = connection.execute(
                "SELECT * FROM profiles WHERE username = ? COLLATE NOCASE", (username,)
            ).fetchone()
        if row is None:
            raise ValueError("Unable to create local community profile")
        return dict(row)

    @staticmethod
    def _validate_local_username(username):
        username = str(username).strip()
        length = len(username.encode("utf-16le")) // 2
        if (not username or length > 31 or
                any(ord(character) < 32 or character in "\t\r\n" for character in username)):
            raise ValueError("Local username must contain 1 to 31 printable characters")
        return username

    def reserve_local_account(self, username, user_id):
        username = self._validate_local_username(username)
        now = int(time.time())
        with self.lock, self._connection() as connection:
            connection.execute("""
                INSERT INTO profiles (user_id, username, display_name, graphic,
                                      registered_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(username) DO NOTHING
            """, (int(user_id), username, username, DEFAULT_PROFILE_GRAPHIC, now, now))
            cursor = connection.execute("""
                INSERT OR IGNORE INTO auth_accounts (username, salt, password_hash, created_at)
                VALUES (?, NULL, NULL, ?)
            """, (username, now))
            if cursor.rowcount == 0:
                raise ValueError("A local account with this username already exists")
        return self.get_profile(username)

    @staticmethod
    def _new_password_hash(password):
        password_bytes = str(password).encode("utf-8")
        if not password_bytes or len(password_bytes) > 1024:
            raise ValueError("Local password must contain 1 to 1024 UTF-8 bytes")
        salt = secrets.token_bytes(16)
        return salt, hashlib.pbkdf2_hmac(
            "sha256", password_bytes, salt, LOCAL_PASSWORD_ITERATIONS, dklen=32)

    def create_local_account(self, username, user_id, password):
        username = self._validate_local_username(username)
        salt, password_hash = self._new_password_hash(password)
        now = int(time.time())
        with self.lock, self._connection() as connection:
            connection.execute("""
                INSERT INTO profiles (user_id, username, display_name, graphic,
                                      registered_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(username) DO NOTHING
            """, (int(user_id), username, username, DEFAULT_PROFILE_GRAPHIC, now, now))
            try:
                connection.execute("""
                    INSERT INTO auth_accounts (username, salt, password_hash, iterations, created_at)
                    VALUES (?, ?, ?, ?, ?)
                """, (username, salt, password_hash, LOCAL_PASSWORD_ITERATIONS, now))
            except sqlite3.IntegrityError as exc:
                raise ValueError("A local account with this username already exists") from exc
        return self.get_profile(username)

    def local_account_exists(self, username):
        with self.lock, self._connection() as connection:
            row = connection.execute(
                "SELECT 1 FROM auth_accounts WHERE username = ? COLLATE NOCASE",
                (str(username),)).fetchone()
        return row is not None

    def local_account_usernames(self):
        with self.lock, self._connection() as connection:
            rows = connection.execute(
                "SELECT username FROM auth_accounts ORDER BY lower(username)").fetchall()
        return tuple(row["username"] for row in rows)

    def verify_local_account(self, username, password):
        username = str(username).strip()
        password_bytes = str(password).encode("utf-8")
        if not password_bytes or len(password_bytes) > 1024:
            return "invalid", False
        with self.lock, self._connection() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT salt, password_hash, iterations FROM auth_accounts "
                "WHERE username = ? COLLATE NOCASE", (username,)).fetchone()
            if row is None:
                return "account_missing", False
            iterations = max(100_000, int(row["iterations"]))
            if row["salt"] is None or row["password_hash"] is None:
                salt = secrets.token_bytes(16)
                password_hash = hashlib.pbkdf2_hmac(
                    "sha256", password_bytes, salt, iterations, dklen=32)
                connection.execute("""
                    UPDATE auth_accounts SET salt = ?, password_hash = ?, iterations = ?
                    WHERE username = ? COLLATE NOCASE AND salt IS NULL AND password_hash IS NULL
                """, (salt, password_hash, iterations, username))
                return "valid", True
            password_hash = hashlib.pbkdf2_hmac(
                "sha256", password_bytes, bytes(row["salt"]), iterations, dklen=32)
            return ("valid" if hmac.compare_digest(password_hash, bytes(row["password_hash"]))
                    else "invalid"), False

    def set_local_password(self, username, password):
        salt, password_hash = self._new_password_hash(password)
        with self.lock, self._connection() as connection:
            cursor = connection.execute("""
                UPDATE auth_accounts SET salt = ?, password_hash = ?, iterations = ?
                WHERE username = ? COLLATE NOCASE
            """, (salt, password_hash, LOCAL_PASSWORD_ITERATIONS, str(username)))
            if cursor.rowcount == 0:
                raise ValueError("No local account with this username")

    def set_account_info(self, username, provider, email, send_info):
        email = str(email).strip()
        if len(email) > 255 or any(ord(character) < 32 for character in email):
            raise ValueError("Email must be at most 255 printable characters")
        now = int(time.time())
        with self.lock, self._connection() as connection:
            connection.execute("""
                INSERT INTO account_info (username, provider, email, send_info,
                                          created_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(username) DO UPDATE SET
                    provider = excluded.provider, email = excluded.email,
                    send_info = excluded.send_info, updated_at = excluded.updated_at
            """, (str(username), str(provider), email, 1 if send_info else 0, now, now))

    def account_info(self, username):
        with self.lock, self._connection() as connection:
            row = connection.execute(
                "SELECT * FROM account_info WHERE username = ? COLLATE NOCASE",
                (str(username),)).fetchone()
        return dict(row) if row is not None else None

    def usernames_for_email(self, email):
        with self.lock, self._connection() as connection:
            rows = connection.execute(
                "SELECT username FROM account_info WHERE email = ? COLLATE NOCASE "
                "ORDER BY lower(username)", (str(email).strip(),)).fetchall()
        return tuple(row["username"] for row in rows)

    def get_profile(self, username, user_id=None):
        row = None
        with self.lock, self._connection() as connection:
            row = connection.execute(
                "SELECT * FROM profiles WHERE username = ? COLLATE NOCASE", (str(username),)
            ).fetchone()
        if row is None:
            if user_id is None:
                return None
            return self.ensure_profile(username, user_id)
        return dict(row)

    def get_profile_by_user_id(self, user_id):
        with self.lock, self._connection() as connection:
            row = connection.execute(
                "SELECT * FROM profiles WHERE user_id = ?", (int(user_id),)
            ).fetchone()
        return dict(row) if row is not None else None

    def update_profile(self, username, user_id, **fields):
        unknown = set(fields) - self.PROFILE_FIELDS
        if unknown:
            raise ValueError(f"Unsupported profile fields: {', '.join(sorted(unknown))}")
        profile = self.ensure_profile(username, user_id)
        if not fields:
            return profile
        updates = {name: (int(value) if name == "appending" else str(value))
                   for name, value in fields.items()}
        updates["updated_at"] = int(time.time())
        assignments = ", ".join(f"{name} = ?" for name in updates)
        with self.lock, self._connection() as connection:
            connection.execute(
                f"UPDATE profiles SET {assignments} WHERE user_id = ?",
                (*updates.values(), profile["user_id"]))
        return self.get_profile(username)

    def search_players(self, term="", limit=100):
        limit = max(0, min(int(limit), 100))
        term = str(term).strip().casefold()
        with self.lock, self._connection() as connection:
            rows = connection.execute(
                "SELECT user_id, username, display_name FROM profiles "
                "WHERE instr(lower(username), ?) > 0 OR instr(lower(display_name), ?) > 0 "
                "ORDER BY lower(display_name), user_id LIMIT ?",
                (term, term, limit)).fetchall()
        return [dict(row) for row in rows]

    def create_tribe(self, name, tag, appending, recruiting, description, founder_username,
                     founder_user_id):
        name = str(name).strip()
        tag = str(tag).strip()
        if not name or not tag:
            raise ValueError("Tribe name and tag are required")
        now = int(time.time())
        self.ensure_profile(founder_username, founder_user_id)
        try:
            with self.lock, self._connection() as connection:
                cursor = connection.execute("""
                    INSERT INTO tribes (name, tag, appending, recruiting, description, created_at)
                    VALUES (?, ?, ?, ?, ?, ?)
                """, (name, tag, int(bool(appending)), int(bool(recruiting)), str(description), now))
                tribe_id = cursor.lastrowid
                connection.execute("""
                    INSERT INTO memberships (tribe_id, user_id, access, title, joined_at)
                    VALUES (?, ?, 4, 'Founder', ?)
                """, (tribe_id, int(founder_user_id), now))
                connection.execute("""
                    UPDATE profiles SET primary_tribe = ?
                    WHERE user_id = ? AND primary_tribe = ''
                """, (name, int(founder_user_id)))
        except sqlite3.IntegrityError as exc:
            raise ValueError("Tribe already exists") from exc
        return self.get_tribe(name)

    def get_tribe(self, name):
        with self.lock, self._connection() as connection:
            row = connection.execute(
                "SELECT * FROM tribes WHERE name = ? COLLATE NOCASE", (str(name),)
            ).fetchone()
        return dict(row) if row is not None else None

    def update_tribe(self, name, **fields):
        unknown = set(fields) - self.TRIBE_FIELDS
        if unknown:
            raise ValueError(f"Unsupported tribe fields: {', '.join(sorted(unknown))}")
        if not fields:
            return self.get_tribe(name)
        updates = {field: (int(bool(value)) if field in {"appending", "recruiting"}
                           else str(value)) for field, value in fields.items()}
        assignments = ", ".join(f"{field} = ?" for field in updates)
        with self.lock, self._connection() as connection:
            connection.execute(f"UPDATE tribes SET {assignments} WHERE name = ? COLLATE NOCASE",
                               (*updates.values(), str(name)))
        return self.get_tribe(name)

    def delete_tribe(self, name):
        with self.lock, self._connection() as connection:
            cursor = connection.execute("DELETE FROM tribes WHERE name = ? COLLATE NOCASE",
                                        (str(name),))
        return cursor.rowcount > 0

    def search_tribes(self, term="", limit=100):
        term = str(term).strip().casefold()
        limit = max(0, min(int(limit), 100))
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT t.tribe_id, t.name, t.tag, t.recruiting, t.appending,
                       COUNT(m.user_id) AS member_count
                FROM tribes AS t LEFT JOIN memberships AS m USING (tribe_id)
                WHERE instr(lower(t.name), ?) > 0 OR instr(lower(t.tag), ?) > 0
                GROUP BY t.tribe_id
                ORDER BY lower(t.name), t.tribe_id LIMIT ?
            """, (term, term, limit)).fetchall()
        return [dict(row) for row in rows]

    def list_tribe_members(self, name, limit=100):
        limit = max(0, min(int(limit), 100))
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT p.user_id, p.username, p.display_name, m.access, m.title, m.joined_at
                FROM memberships AS m
                JOIN tribes AS t USING (tribe_id)
                JOIN profiles AS p USING (user_id)
                WHERE t.name = ? COLLATE NOCASE
                ORDER BY m.access DESC, lower(p.display_name) LIMIT ?
            """, (str(name), limit)).fetchall()
        return [dict(row) for row in rows]

    def list_profile_tribes(self, username, limit=100):
        limit = max(0, min(int(limit), 100))
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT t.tribe_id, t.name, t.tag, t.appending, m.access, m.title, m.joined_at
                FROM memberships AS m
                JOIN tribes AS t USING (tribe_id)
                JOIN profiles AS p USING (user_id)
                WHERE p.username = ? COLLATE NOCASE
                ORDER BY m.access DESC, lower(t.name) LIMIT ?
            """, (str(username), limit)).fetchall()
        return [dict(row) for row in rows]

    def tribe_by_id(self, tribe_id):
        with self.lock, self._connection() as connection:
            row = connection.execute("SELECT * FROM tribes WHERE tribe_id = ?", (int(tribe_id),)).fetchone()
        return dict(row) if row is not None else None

    def membership(self, tribe_name, username):
        with self.lock, self._connection() as connection:
            row = connection.execute("""
                SELECT m.* FROM memberships AS m
                JOIN tribes AS t USING (tribe_id)
                JOIN profiles AS p USING (user_id)
                WHERE t.name = ? COLLATE NOCASE AND p.username = ? COLLATE NOCASE
            """, (str(tribe_name), str(username))).fetchone()
        return dict(row) if row is not None else None

    def update_membership(self, tribe_name, username, title=None, access=None):
        membership = self.membership(tribe_name, username)
        if membership is None:
            raise ValueError("Tribe membership not found")
        updates = {}
        if title is not None:
            updates["title"] = str(title)
        if access is not None:
            updates["access"] = int(access)
        if updates:
            assignments = ", ".join(f"{field} = ?" for field in updates)
            with self.lock, self._connection() as connection:
                connection.execute(
                    f"UPDATE memberships SET {assignments} WHERE tribe_id = ? AND user_id = ?",
                    (*updates.values(), membership["tribe_id"], membership["user_id"]))
        return self.membership(tribe_name, username)

    def remove_membership(self, tribe_name, username):
        membership = self.membership(tribe_name, username)
        if membership is None:
            return False
        with self.lock, self._connection() as connection:
            connection.execute("DELETE FROM memberships WHERE tribe_id = ? AND user_id = ?",
                               (membership["tribe_id"], membership["user_id"]))
        return True

    def invite(self, tribe_name, username, user_id, inviter_username=None, inviter_user_id=None):
        tribe = self.get_tribe(tribe_name)
        if tribe is None:
            raise ValueError("Tribe not found")
        if self.membership(tribe_name, username) is not None:
            raise ValueError("Player is already a member of this tribe")
        profile = self.ensure_profile(username, user_id)
        inviter = (self.ensure_profile(inviter_username, inviter_user_id)
                   if inviter_username is not None and inviter_user_id is not None else None)
        with self.lock, self._connection() as connection:
            inviter_id = (inviter["user_id"] if inviter is not None else connection.execute(
                "SELECT user_id FROM memberships WHERE tribe_id = ? AND access >= 4 "
                "ORDER BY access DESC, joined_at LIMIT 1", (tribe["tribe_id"],)).fetchone())
            if isinstance(inviter_id, sqlite3.Row):
                inviter_id = inviter_id["user_id"]
            connection.execute("""
                INSERT INTO invitations (tribe_id, user_id, inviter_id, status, created_at)
                VALUES (?, ?, ?, 'pending', ?)
                ON CONFLICT(tribe_id, user_id) DO UPDATE SET
                    inviter_id=excluded.inviter_id, status='pending', created_at=excluded.created_at
            """, (tribe["tribe_id"], profile["user_id"], inviter_id, int(time.time())))
        return True

    def request_invitation(self, tribe_name, username, user_id):
        tribe = self.get_tribe(tribe_name)
        if tribe is None:
            raise ValueError("Tribe not found")
        profile = self.ensure_profile(username, user_id)
        if self.membership(tribe_name, username) is not None:
            raise ValueError("Already a member of this tribe")
        with self.lock, self._connection() as connection:
            existing = connection.execute("""
                SELECT status FROM invitations WHERE tribe_id = ? AND user_id = ?
            """, (tribe["tribe_id"], profile["user_id"])).fetchone()
            if existing is not None and existing["status"] in {"pending", "requested"}:
                raise ValueError("An invitation or request already exists")
            connection.execute("""
                INSERT INTO invitations (tribe_id, user_id, status, created_at)
                VALUES (?, ?, 'requested', ?)
                ON CONFLICT(tribe_id, user_id) DO UPDATE
                    SET status='requested', created_at=excluded.created_at
            """, (tribe["tribe_id"], profile["user_id"], int(time.time())))
        return True

    def respond_to_invite(self, action, tribe_name, username, user_id,
                          expected_status="pending"):
        tribe = self.get_tribe(tribe_name)
        if tribe is None:
            raise ValueError("Tribe not found")
        profile = self.ensure_profile(username, user_id)
        with self.lock, self._connection() as connection:
            if isinstance(expected_status, (tuple, list, set)):
                expected_status = tuple(expected_status)
                status_clause = "status IN (" + ",".join("?" for _ in expected_status) + ")"
                status_values = expected_status
            else:
                status_clause = "status = ?"
                status_values = (str(expected_status),)
            invitation = connection.execute("""
                SELECT invite_id FROM invitations
                WHERE tribe_id = ? AND user_id = ? AND """ + status_clause,
                (tribe["tribe_id"], profile["user_id"], *status_values)).fetchone()
            if invitation is None:
                raise ValueError("Tribe invitation not found")
            if action == "accept":
                connection.execute("""
                    INSERT OR IGNORE INTO memberships (tribe_id, user_id, access, title, joined_at)
                    VALUES (?, ?, 1, 'Member', ?)
                """, (tribe["tribe_id"], profile["user_id"], int(time.time())))
                status = "accepted"
            elif action == "reject":
                status = "rejected"
            elif action == "cancel":
                status = "cancelled"
            else:
                raise ValueError("Unknown tribe invitation action")
            connection.execute("UPDATE invitations SET status=? WHERE invite_id=?",
                               (status, invitation["invite_id"]))
        return status

    def list_invitations(self, tribe_name):
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT i.invite_id, i.created_at, i.status,
                       invited.username AS invited_name, invited.user_id AS invited_id,
                       invited.tag AS invited_tag, invited.appending AS invited_appending,
                        inviter.username AS inviter_name, inviter.user_id AS inviter_id,
                        inviter.tag AS inviter_tag, inviter.appending AS inviter_appending
                FROM invitations AS i
                JOIN tribes AS t ON t.tribe_id = i.tribe_id
                JOIN profiles AS invited ON invited.user_id = i.user_id
                LEFT JOIN profiles AS inviter ON inviter.user_id = COALESCE(
                    i.inviter_id,
                    (SELECT fm.user_id FROM memberships AS fm
                     WHERE fm.tribe_id = t.tribe_id AND fm.access >= 4
                     ORDER BY fm.access DESC, fm.joined_at LIMIT 1))
                WHERE t.name = ? COLLATE NOCASE AND i.status IN ('pending', 'requested')
                ORDER BY i.created_at, i.invite_id
            """, (str(tribe_name),)).fetchall()
        return [dict(row) for row in rows]

    def send_mail(self, sender_username, sender_user_id, recipients, subject, body,
                  to_list="", cc_list=""):
        sender = self.ensure_profile(sender_username, sender_user_id)
        normalized = {}
        for username, is_cc in recipients:
            username = str(username).strip()
            if not username:
                continue
            profile = self.get_profile(username)
            if profile is None:
                raise ValueError(f"Player not found: {username}")
            normalized[profile["user_id"]] = (profile, bool(is_cc))
        if not normalized:
            raise ValueError("At least one valid recipient is required")
        now = int(time.time())
        with self.lock, self._connection() as connection:
            allowed = [(profile, is_cc) for profile, is_cc in normalized.values()
                       if connection.execute("""
                           SELECT 1 FROM email_blocks WHERE owner_id = ? AND blocked_id = ?
                       """, (profile["user_id"], sender["user_id"])).fetchone() is None]
            connection.executemany("""
                INSERT INTO mail (sender_id, recipient_id, is_cc, to_list, cc_list,
                                  subject, body, created_at)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?)
            """, [(sender["user_id"], profile["user_id"], int(is_cc), str(to_list),
                   str(cc_list), str(subject), str(body), now) for profile, is_cc in allowed])
        return len(allowed)

    def inbox(self, username, after_id=0, limit=100, include_deleted=False, deleted_only=False):
        limit = max(0, min(int(limit), 100))
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT m.mail_id, m.is_cc, m.to_list, m.cc_list, m.subject, m.body, m.created_at,
                       m.is_read, m.is_deleted, sender.username AS sender_name,
                       sender.user_id AS sender_id, sender.tag AS sender_tag,
                       sender.appending AS sender_appending,
                       recipient.username AS recipient_name, recipient.user_id AS recipient_id,
                       recipient.tag AS recipient_tag, recipient.appending AS recipient_appending
                FROM mail AS m
                JOIN profiles AS sender ON sender.user_id = m.sender_id
                JOIN profiles AS recipient ON recipient.user_id = m.recipient_id
                WHERE recipient.username = ? COLLATE NOCASE AND m.mail_id > ?
                      AND (? OR m.is_deleted = 0)
                      AND (NOT ? OR m.is_deleted = 1)
                ORDER BY m.mail_id LIMIT ?
            """, (str(username), int(after_id), int(bool(include_deleted)),
                  int(bool(deleted_only)), limit)).fetchall()
        return [dict(row) for row in rows]

    def update_mail(self, username, mail_id, *, is_read=None, is_deleted=None):
        updates = {}
        if is_read is not None:
            updates["is_read"] = int(bool(is_read))
        if is_deleted is not None:
            updates["is_deleted"] = int(bool(is_deleted))
        if not updates:
            return False
        assignments = ", ".join(f"{field} = ?" for field in updates)
        with self.lock, self._connection() as connection:
            cursor = connection.execute(
                f"UPDATE mail SET {assignments} WHERE mail_id = ? AND recipient_id = "
                "(SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)",
                (*updates.values(), int(mail_id), str(username)))
        return cursor.rowcount > 0

    def email_block_list(self, username):
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT p.username, p.user_id, p.tag, p.appending, 0 AS blocked_count
                FROM email_blocks AS eb
                JOIN profiles AS owner ON owner.user_id = eb.owner_id
                JOIN profiles AS p ON p.user_id = eb.blocked_id
                WHERE owner.username = ? COLLATE NOCASE
                ORDER BY lower(p.username)
            """, (str(username),)).fetchall()
        return [dict(row) for row in rows]

    def set_email_block(self, owner_username, blocked_username):
        with self.lock, self._connection() as connection:
            owner = connection.execute(
                "SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE",
                (str(owner_username),)).fetchone()
            blocked = connection.execute(
                "SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE",
                (str(blocked_username),)).fetchone()
            if owner is None or blocked is None:
                raise ValueError("Player not found")
            connection.execute("""
                INSERT OR IGNORE INTO email_blocks (owner_id, blocked_id, created_at)
                VALUES (?, ?, ?)
            """, (owner["user_id"], blocked["user_id"], int(time.time())))
        return True

    def remove_email_block(self, owner_username, blocked_username):
        with self.lock, self._connection() as connection:
            cursor = connection.execute("""
                DELETE FROM email_blocks WHERE owner_id = (
                    SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)
                  AND blocked_id = (
                    SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)
            """, (str(owner_username), str(blocked_username)))
        return cursor.rowcount > 0

    def tribesnext_picture(self, guid):
        with self.lock, self._connection() as connection:
            row = connection.execute(
                "SELECT graphic FROM tribesnext_pictures WHERE guid = ?", (int(guid),)
            ).fetchone()
        return row["graphic"] if row is not None else None

    def set_tribesnext_picture(self, guid, graphic):
        with self.lock, self._connection() as connection:
            connection.execute("""
                INSERT INTO tribesnext_pictures (guid, graphic, updated_at) VALUES (?, ?, ?)
                ON CONFLICT(guid) DO UPDATE SET graphic = excluded.graphic,
                                                updated_at = excluded.updated_at
            """, (int(guid), str(graphic), int(time.time())))

    def list_buddies(self, owner_username):
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT p.user_id, p.username, p.display_name, p.tag, p.appending
                FROM buddies AS b
                JOIN profiles AS owner ON owner.user_id = b.owner_id
                JOIN profiles AS p ON p.user_id = b.buddy_id
                WHERE owner.username = ? COLLATE NOCASE
                ORDER BY lower(p.display_name), p.user_id
            """, (str(owner_username),)).fetchall()
        return [dict(row) for row in rows]

    def add_buddy(self, owner_username, owner_user_id, buddy_username, buddy_user_id):
        owner = self.ensure_profile(owner_username, owner_user_id)
        buddy = self.ensure_profile(buddy_username, buddy_user_id)
        with self.lock, self._connection() as connection:
            connection.execute("""
                INSERT OR IGNORE INTO buddies (owner_id, buddy_id, created_at)
                VALUES (?, ?, ?)
            """, (owner["user_id"], buddy["user_id"], int(time.time())))
        return True

    def remove_buddy(self, owner_username, buddy_username):
        with self.lock, self._connection() as connection:
            cursor = connection.execute("""
                DELETE FROM buddies WHERE owner_id = (
                    SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)
                  AND buddy_id = (
                    SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)
            """, (str(owner_username), str(buddy_username)))
        return cursor.rowcount > 0

    def clear_buddies(self, owner_username):
        with self.lock, self._connection() as connection:
            cursor = connection.execute("""
                DELETE FROM buddies WHERE owner_id = (
                    SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)
            """, (str(owner_username),))
        return cursor.rowcount

    def forum_list(self, username=None):
        with self.lock, self._connection() as connection:
            rows = connection.execute(
                "SELECT forum_id, name, flags, tribe_id FROM forums ORDER BY forum_id").fetchall()
            tribes = []
            if username is not None:
                tribes = connection.execute("""
                    SELECT -t.tribe_id AS forum_id, t.name, 1 AS flags, t.tribe_id
                    FROM memberships AS m
                    JOIN profiles AS p ON p.user_id = m.user_id
                    JOIN tribes AS t ON t.tribe_id = m.tribe_id
                    WHERE p.username = ? COLLATE NOCASE
                    ORDER BY lower(t.name)
                """, (str(username),)).fetchall()
        return [dict(row) for row in rows] + [dict(row) for row in tribes]

    def ensure_forum(self, forum_id):
        forum_id = int(forum_id)
        if forum_id < 0:
            tribe = self.tribe_by_id(-forum_id)
            if tribe is None:
                raise ValueError("Tribe forum not found")
            with self.lock, self._connection() as connection:
                connection.execute("""
                    INSERT OR IGNORE INTO forums (forum_id, name, flags, tribe_id)
                    VALUES (?, ?, 1, ?)
                """, (forum_id, tribe["name"], tribe["tribe_id"]))
        with self.lock, self._connection() as connection:
            row = connection.execute(
                "SELECT forum_id, name, flags, tribe_id FROM forums WHERE forum_id = ?",
                (forum_id,)).fetchone()
        if row is None:
            raise ValueError("Forum not found")
        return dict(row)

    def create_forum_post(self, forum_id, topic_id, parent_id, author_username, author_user_id,
                          subject, body):
        self.ensure_forum(forum_id)
        profile = self.ensure_profile(author_username, author_user_id)
        now = int(time.time())
        subject = str(subject).strip()
        if not subject:
            raise ValueError("Forum subject cannot be empty")
        if topic_id:
            with self.lock, self._connection() as connection:
                topic = connection.execute(
                    "SELECT topic_id, forum_id, locked FROM forum_topics "
                    "WHERE topic_id = ? AND is_deleted = 0",
                    (int(topic_id),)).fetchone()
                if topic is None and not parent_id:
                    topic_id = 0
                elif topic is None or topic["forum_id"] != int(forum_id):
                    raise ValueError("Forum topic not found")
                if topic_id and topic["locked"]:
                    raise ValueError("Forum topic is locked")
                if topic_id and parent_id:
                    parent = connection.execute(
                        "SELECT post_id FROM forum_posts WHERE post_id = ? AND topic_id = ? AND is_deleted = 0",
                        (int(parent_id), int(topic_id))).fetchone()
                    if parent is None:
                        raise ValueError("Parent forum post not found")
                if topic_id:
                    update_id = self._next_forum_update(connection)
                    cursor = connection.execute("""
                        INSERT INTO forum_posts (topic_id, parent_id, author_id, subject, body,
                                                 created_at, updated_at, update_id)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                    """, (int(topic_id), int(parent_id), profile["user_id"], subject,
                          str(body), now, now, update_id))
                    post_id = cursor.lastrowid
                    connection.execute(
                        "UPDATE forum_topics SET updated_at = ?, update_id = ? WHERE topic_id = ?",
                        (now, update_id, int(topic_id)))
                else:
                    forum = connection.execute(
                        "SELECT forum_id FROM forums WHERE forum_id = ?", (int(forum_id),)).fetchone()
                    if forum is None:
                        raise ValueError("Forum not found")
                    update_id = self._next_forum_update(connection)
                    cursor = connection.execute("""
                        INSERT INTO forum_topics (forum_id, subject, author_id, created_at,
                                                  updated_at, update_id)
                        VALUES (?, ?, ?, ?, ?, ?)
                    """, (int(forum_id), subject, profile["user_id"], now, now, update_id))
                    topic_id = cursor.lastrowid
                    post_cursor = connection.execute("""
                        INSERT INTO forum_posts (topic_id, parent_id, author_id, subject, body,
                                                 created_at, updated_at, update_id)
                        VALUES (?, 0, ?, ?, ?, ?, ?, ?)
                    """, (topic_id, profile["user_id"], subject, str(body), now, now, update_id))
                    post_id = post_cursor.lastrowid
        else:
            with self.lock, self._connection() as connection:
                forum = connection.execute(
                    "SELECT forum_id FROM forums WHERE forum_id = ?", (int(forum_id),)).fetchone()
                if forum is None:
                    raise ValueError("Forum not found")
                update_id = self._next_forum_update(connection)
                cursor = connection.execute("""
                    INSERT INTO forum_topics (forum_id, subject, author_id, created_at,
                                              updated_at, update_id)
                    VALUES (?, ?, ?, ?, ?, ?)
                """, (int(forum_id), subject, profile["user_id"], now, now, update_id))
                topic_id = cursor.lastrowid
                post_cursor = connection.execute("""
                    INSERT INTO forum_posts (topic_id, parent_id, author_id, subject, body,
                                             created_at, updated_at, update_id)
                    VALUES (?, 0, ?, ?, ?, ?, ?, ?)
                """, (topic_id, profile["user_id"], subject, str(body), now, now, update_id))
                post_id = post_cursor.lastrowid
        return {"topic_id": int(topic_id), "post_id": int(post_id)}

    def list_forum_topics(self, forum_id, offset=0, limit=60):
        self.ensure_forum(forum_id)
        offset = max(0, int(offset))
        limit = max(0, min(int(limit), 100))
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT t.topic_id, t.forum_id, t.subject, t.created_at, t.updated_at,
                       t.update_id,
                       t.is_deleted, t.locked, p.username AS author_name,
                       (SELECT COUNT(*) FROM forum_posts AS fp
                        WHERE fp.topic_id = t.topic_id AND fp.is_deleted = 0) AS post_count
                FROM forum_topics AS t
                JOIN profiles AS p ON p.user_id = t.author_id
                WHERE t.forum_id = ? AND t.is_deleted = 0
                ORDER BY t.update_id DESC, t.topic_id DESC LIMIT ? OFFSET ?
            """, (int(forum_id), limit, offset)).fetchall()
        return [dict(row) for row in rows]

    def list_forum_posts(self, topic_id, after_update_id=0, limit=100):
        limit = max(0, min(int(limit), 100))
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT fp.post_id, fp.topic_id, fp.parent_id, fp.subject, fp.body,
                       fp.created_at, fp.updated_at, fp.update_id, fp.is_deleted,
                       p.username AS author_name, p.user_id AS author_id,
                       p.tag AS author_tag, p.appending AS author_appending
                FROM forum_posts AS fp
                JOIN profiles AS p ON p.user_id = fp.author_id
                WHERE fp.topic_id = ? AND fp.update_id > ?
                ORDER BY fp.update_id, fp.post_id LIMIT ?
            """, (int(topic_id), int(after_update_id), limit)).fetchall()
        return [dict(row) for row in rows]

    def forum_for_topic(self, topic_id):
        with self.lock, self._connection() as connection:
            row = connection.execute(
                "SELECT f.* FROM forums AS f JOIN forum_topics AS t USING (forum_id) "
                "WHERE t.topic_id = ?", (int(topic_id),)).fetchone()
        return dict(row) if row is not None else None

    def forum_for_post(self, post_id):
        with self.lock, self._connection() as connection:
            row = connection.execute("""
                SELECT f.* FROM forums AS f
                JOIN forum_topics AS t USING (forum_id)
                JOIN forum_posts AS p USING (topic_id)
                WHERE p.post_id = ?
            """, (int(post_id),)).fetchone()
        return dict(row) if row is not None else None

    def set_forum_topic_locked(self, topic_id, locked, reason=""):
        with self.lock, self._connection() as connection:
            update_id = self._next_forum_update(connection)
            cursor = connection.execute("""
                UPDATE forum_topics SET locked = ?, lock_reason = ?, updated_at = ?, update_id = ?
                WHERE topic_id = ? AND is_deleted = 0
            """, (int(bool(locked)), str(reason) if locked else "", int(time.time()),
                  update_id, int(topic_id)))
        if cursor.rowcount == 0:
            raise ValueError("Forum topic not found")
        return True

    def move_forum_topic(self, topic_id, target_forum_id):
        self.ensure_forum(target_forum_id)
        with self.lock, self._connection() as connection:
            update_id = self._next_forum_update(connection)
            cursor = connection.execute("""
                UPDATE forum_topics SET forum_id = ?, updated_at = ?, update_id = ?
                WHERE topic_id = ? AND is_deleted = 0
            """, (int(target_forum_id), int(time.time()), update_id, int(topic_id)))
        if cursor.rowcount == 0:
            raise ValueError("Forum topic not found")
        return True

    def delete_forum_topic(self, topic_id):
        now = int(time.time())
        with self.lock, self._connection() as connection:
            update_id = self._next_forum_update(connection)
            cursor = connection.execute("""
                UPDATE forum_topics SET is_deleted = 1, updated_at = ?, update_id = ?
                WHERE topic_id = ? AND is_deleted = 0
            """, (now, update_id, int(topic_id)))
            if cursor.rowcount:
                connection.execute("""
                    UPDATE forum_posts SET is_deleted = 1, updated_at = ?, update_id = ?
                    WHERE topic_id = ?
                """, (now, update_id, int(topic_id)))
        return cursor.rowcount > 0

    def edit_forum_post(self, post_id, author_username, subject, body):
        with self.lock, self._connection() as connection:
            update_id = self._next_forum_update(connection)
            cursor = connection.execute("""
                UPDATE forum_posts SET subject = ?, body = ?, updated_at = ?, update_id = ?
                WHERE post_id = ? AND author_id = (
                    SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)
                      AND is_deleted = 0
            """, (str(subject), str(body), int(time.time()), update_id,
                  int(post_id), str(author_username)))
            if cursor.rowcount:
                connection.execute("""
                    UPDATE forum_topics SET subject = ?
                    WHERE topic_id = (SELECT topic_id FROM forum_posts WHERE post_id = ?)
                      AND ? = (SELECT MIN(post_id) FROM forum_posts
                               WHERE topic_id = forum_topics.topic_id)
                """, (str(subject), int(post_id), int(post_id)))
                connection.execute("""
                    UPDATE forum_topics SET updated_at = ?, update_id = ?
                    WHERE topic_id = (SELECT topic_id FROM forum_posts WHERE post_id = ?)
                """, (int(time.time()), update_id, int(post_id)))
        return cursor.rowcount > 0

    def delete_forum_post(self, post_id, author_username):
        with self.lock, self._connection() as connection:
            update_id = self._next_forum_update(connection)
            cursor = connection.execute("""
                UPDATE forum_posts SET is_deleted = 1, updated_at = ?, update_id = ?
                WHERE post_id = ? AND author_id = (
                    SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)
                      AND is_deleted = 0
            """, (int(time.time()), update_id, int(post_id), str(author_username)))
            if cursor.rowcount:
                connection.execute("""
                    UPDATE forum_topics SET updated_at = ?, update_id = ?
                    WHERE topic_id = (SELECT topic_id FROM forum_posts WHERE post_id = ?)
                """, (int(time.time()), update_id, int(post_id)))
        return cursor.rowcount > 0

    def post_news(self, category_id, author_username, author_user_id, title, body):
        category_id = int(category_id)
        if category_id not in {105, 35500, 35501, 35503, 35504}:
            raise ValueError("Unknown news category")
        title = str(title).strip()
        if not title:
            raise ValueError("News title cannot be empty")
        profile = self.ensure_profile(author_username, author_user_id)
        now = int(time.time())
        with self.lock, self._connection() as connection:
            cursor = connection.execute("""
                INSERT INTO news_articles (category_id, author_id, title, body, created_at, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
            """, (category_id, profile["user_id"], title, str(body), now, now))
        return int(cursor.lastrowid)

    def list_news(self, category_id, offset=0, limit=23):
        category_id = int(category_id)
        offset = max(0, int(offset))
        limit = max(0, min(int(limit), 100))
        with self.lock, self._connection() as connection:
            rows = connection.execute("""
                SELECT n.article_id, n.category_id, n.title, n.body,
                       n.created_at, n.updated_at, p.username AS author_name,
                       p.user_id AS author_id, p.tag AS author_tag,
                       p.appending AS author_appending
                FROM news_articles AS n
                JOIN profiles AS p ON p.user_id = n.author_id
                WHERE n.category_id = ? AND n.is_deleted = 0
                ORDER BY n.created_at DESC, n.article_id DESC LIMIT ? OFFSET ?
            """, (category_id, limit, offset)).fetchall()
            count = connection.execute(
                "SELECT COUNT(*) FROM news_articles WHERE category_id = ? AND is_deleted = 0",
                (category_id,)).fetchone()[0]
        return [dict(row) for row in rows], int(count)

    def edit_news(self, article_id, author_username, category_id, title, body):
        if int(category_id) not in {105, 35500, 35501, 35503, 35504}:
            raise ValueError("Unknown news category")
        with self.lock, self._connection() as connection:
            cursor = connection.execute("""
                UPDATE news_articles SET category_id = ?, title = ?, body = ?, updated_at = ?
                WHERE article_id = ? AND author_id = (
                    SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)
                      AND is_deleted = 0
            """, (int(category_id), str(title), str(body), int(time.time()),
                  int(article_id), str(author_username)))
        return cursor.rowcount > 0

    def delete_news(self, article_id, author_username):
        with self.lock, self._connection() as connection:
            cursor = connection.execute("""
                UPDATE news_articles SET is_deleted = 1, updated_at = ?
                WHERE article_id = ? AND author_id = (
                    SELECT user_id FROM profiles WHERE username = ? COLLATE NOCASE)
                      AND is_deleted = 0
            """, (int(time.time()), int(article_id), str(author_username)))
        return cursor.rowcount > 0
