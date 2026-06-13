import sys
import os
import json
import time
import datetime
import tempfile
import subprocess
from pathlib import Path

# Helper directory and venv directory
helper_dir = Path(__file__).parent.resolve()
venv_dir = helper_dir / "venv"

if sys.platform == "win32":
    venv_python = venv_dir / "Scripts" / "python.exe"
else:
    venv_python = venv_dir / "bin" / "python"

# Check if we are running in our local venv
in_venv = (Path(sys.prefix).resolve() == venv_dir.resolve())

if not in_venv:
    try:
        # Create venv if it doesn't exist
        if not venv_dir.exists():
            print("Creating local virtual environment (venv) inside fh6-radio...", file=sys.stderr)
            subprocess.check_call([sys.executable, "-m", "venv", str(venv_dir)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        
        # Check if tidalapi is installed inside the venv
        try:
            subprocess.check_call([str(venv_python), "-c", "import tidalapi"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            has_tidalapi = True
        except subprocess.CalledProcessError:
            has_tidalapi = False

        if not has_tidalapi:
            print("Installing tidalapi to local virtual environment...", file=sys.stderr)
            subprocess.check_call([str(venv_python), "-m", "pip", "install", "tidalapi"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        
        # Re-execute using the venv python
        res = subprocess.run([str(venv_python), "-u"] + sys.argv, capture_output=False)
        sys.exit(res.returncode)
    except Exception as e:
        print(json.dumps({
            "status": "error", 
            "error": f"Failed to bootstrap virtual environment: {str(e)}"
        }), flush=True)
        sys.exit(1)

# Add local search paths for tidalapi (as fallback)
sys.path.insert(0, str(helper_dir / "python-tidal"))
sys.path.insert(0, str(helper_dir))

try:
    import requests
    import dateutil
    import typing_extensions
    import ratelimit
    import isodate
    import mpegdash
    import pyaes
    import tidalapi
    from tidalapi import Quality
except ImportError as e:
    print(json.dumps({
        "status": "error",
        "error": f"Failed to import tidalapi: {str(e)}"
    }), flush=True)
    sys.exit(1)

def handle_exception(context, exc):
    import requests
    if isinstance(exc, requests.exceptions.HTTPError):
        resp = exc.response
        details = resp.text
        try:
            js = resp.json()
            if "error_description" in js:
                details = js["error_description"]
            elif "error" in js:
                details = js["error"]
            elif "message" in js:
                details = js["message"]
        except:
            pass
        
        err_msg = f"{context}: HTTP {resp.status_code} {resp.reason} ({details}) for URL: {resp.url}"
        print(json.dumps({"status": "error", "error": err_msg}), flush=True)
    else:
        err_msg = f"{context}: {str(exc)}"
        print(json.dumps({"status": "error", "error": err_msg}), flush=True)

def main():
    if len(sys.argv) < 2:
        print(json.dumps({"status": "error", "error": "Missing subcommand (login, refresh, playlist, stream)"}), flush=True)
        sys.exit(1)

    subcommand = sys.argv[1]

    if subcommand == "login":
        client_id = os.environ.get("TIDAL_CLIENT_ID", "")
        client_secret = os.environ.get("TIDAL_CLIENT_SECRET", "")
        if not client_id and len(sys.argv) > 2 and sys.argv[2] != "" and sys.argv[2] != '""':
            client_id = sys.argv[2]
        if not client_secret and len(sys.argv) > 3 and sys.argv[3] != "" and sys.argv[3] != '""':
            client_secret = sys.argv[3]

        session = tidalapi.Session()
        if client_id:
            session.config.client_id = client_id
            if client_secret:
                session.config.client_secret = client_secret
            else:
                session.config.client_secret = client_id

        try:
            login, future = session.login_oauth()
        except Exception as e:
            handle_exception("Failed to start OAuth flow", e)
            sys.exit(1)

        # Print verification URL and user code immediately to stdout for C++ to parse
        print(json.dumps({
            "verification_uri": login.verification_uri_complete,
            "user_code": login.user_code
        }), flush=True)

        # Block until authenticated or timeout
        try:
            future.result()
        except Exception as e:
            handle_exception("Authentication polling failed", e)
            sys.exit(1)

        if session.check_login():
            expires_in = 3600
            if session.expiry_time:
                expires_in = int((session.expiry_time - datetime.datetime.utcnow()).total_seconds())
            print(json.dumps({
                "status": "success",
                "access_token": session.access_token,
                "refresh_token": session.refresh_token,
                "expires_in": expires_in
            }), flush=True)
        else:
            print(json.dumps({"status": "error", "error": "Session verification failed after login"}), flush=True)

    elif subcommand == "refresh":
        refresh_token = os.environ.get("TIDAL_REFRESH_TOKEN", "")
        client_id = os.environ.get("TIDAL_CLIENT_ID", "")
        client_secret = os.environ.get("TIDAL_CLIENT_SECRET", "")

        if not refresh_token and len(sys.argv) > 2:
            refresh_token = sys.argv[2]
        if not client_id and len(sys.argv) > 3 and sys.argv[3] != "" and sys.argv[3] != '""':
            client_id = sys.argv[3]
        if not client_secret and len(sys.argv) > 4 and sys.argv[4] != "" and sys.argv[4] != '""':
            client_secret = sys.argv[4]

        if not refresh_token:
            print(json.dumps({"status": "error", "error": "Missing refresh_token"}), flush=True)
            sys.exit(1)

        session = tidalapi.Session()
        if client_id:
            session.config.client_id = client_id
            if client_secret:
                session.config.client_secret = client_secret
            else:
                session.config.client_secret = client_id

        try:
            success = session.token_refresh(refresh_token)
            if success:
                loaded = session.load_oauth_session(
                    token_type="Bearer",
                    access_token=session.access_token,
                    refresh_token=session.refresh_token,
                    expiry_time=session.expiry_time
                )
                if loaded and session.check_login():
                    expires_in = 3600
                    if session.expiry_time:
                        expires_in = int((session.expiry_time - datetime.datetime.utcnow()).total_seconds())
                    print(json.dumps({
                        "status": "success",
                        "access_token": session.access_token,
                        "refresh_token": session.refresh_token,
                        "expires_in": expires_in
                    }), flush=True)
                else:
                    print(json.dumps({"status": "error", "error": "Token refresh failed session validation"}), flush=True)
            else:
                print(json.dumps({"status": "error", "error": "Token refresh failed"}), flush=True)
        except Exception as e:
            handle_exception("Token refresh failed", e)

    elif subcommand == "playlist":
        access_token = os.environ.get("TIDAL_ACCESS_TOKEN", "")
        if not access_token and len(sys.argv) > 3:
            access_token = sys.argv[3]

        if len(sys.argv) < 3:
            print(json.dumps({"status": "error", "error": "Usage: playlist <playlist_id> [access_token]"}), flush=True)
            sys.exit(1)

        playlist_id = sys.argv[2]

        if not access_token:
            print(json.dumps({"status": "error", "error": "Missing access_token"}), flush=True)
            sys.exit(1)

        session = tidalapi.Session()
        loaded = session.load_oauth_session("Bearer", access_token)
        if not loaded:
            print(json.dumps({"status": "error", "error": "Failed to load OAuth session with provided access token"}), flush=True)
            sys.exit(1)

        try:
            # Try loading as playlist first, fall back to mix if it fails
            try:
                playlist = session.playlist(playlist_id)
                # Cap at 500 tracks to avoid large pagination/parsing delays and memory bloat
                tracks = playlist.tracks(limit=500)
            except Exception as playlist_err:
                try:
                    mix = session.mix(playlist_id)
                    # For mixes, retrieve all items (which are Track or Video objects)
                    tracks = mix.items()
                    # Cap at 500 tracks
                    tracks = tracks[:500]
                except Exception as mix_err:
                    raise Exception(f"Failed as playlist ({str(playlist_err)}) and as mix ({str(mix_err)})")

            tracks_json = []
            for track in tracks:
                # Skip video entries in mixes
                if type(track).__name__ == 'Video':
                    continue

                if hasattr(track, 'artists') and track.artists:
                    artist_name = ", ".join(art.name for art in track.artists if art and art.name)
                elif hasattr(track, 'artist') and track.artist and track.artist.name:
                    artist_name = track.artist.name
                else:
                    artist_name = ""

                album_art = ""
                if hasattr(track, 'album') and track.album:
                    try:
                        album_art = track.album.image(dimensions=320)
                    except:
                        pass

                tracks_json.append({
                    "id": str(track.id),
                    "title": track.name if track.name else "",
                    "artist": artist_name,
                    "album": track.album.name if track.album and track.album.name else "",
                    "artwork_url": album_art,
                    "duration_ms": int(track.duration * 1000) if track.duration else 0
                })

            print(json.dumps({
                "status": "success",
                "tracks": tracks_json
            }), flush=True)
        except Exception as e:
            handle_exception("Failed to fetch tracks", e)

    elif subcommand == "stream":
        access_token = os.environ.get("TIDAL_ACCESS_TOKEN", "")
        
        if len(sys.argv) == 4:
            track_id = sys.argv[2]
            quality_str = sys.argv[3]
        elif len(sys.argv) >= 5:
            track_id = sys.argv[2]
            if not access_token:
                access_token = sys.argv[3]
            quality_str = sys.argv[4]
        else:
            print(json.dumps({"status": "error", "error": "Usage: stream <track_id> <quality>"}), flush=True)
            sys.exit(1)

        if not access_token:
            print(json.dumps({"status": "error", "error": "Missing access_token"}), flush=True)
            sys.exit(1)

        session = tidalapi.Session()
        loaded = session.load_oauth_session("Bearer", access_token)
        if not loaded:
            print(json.dumps({"status": "error", "error": "Failed to load OAuth session with provided access token"}), flush=True)
            sys.exit(1)

        quality_map = {
            "LOW": Quality.low_96k,
            "HIGH": Quality.low_320k,
            "LOSSLESS": Quality.high_lossless,
            "HI_RES": Quality.hi_res_lossless
        }
        target_quality = quality_map.get(quality_str, Quality.low_320k)
        session.audio_quality = target_quality

        try:
            track = session.track(track_id)
            stream = track.get_stream()
            manifest = stream.get_stream_manifest()

            if stream.is_mpd:
                manifest_data = stream.get_manifest_data()
                temp_dir = tempfile.gettempdir()
                temp_file_path = os.path.join(temp_dir, f"tidal_{track_id}.mpd")
                with open(temp_file_path, "w", encoding="utf-8") as f:
                    f.write(manifest_data)
                
                print(json.dumps({
                    "status": "success",
                    "urls": [temp_file_path],
                    "mime_type": "application/dash+xml"
                }), flush=True)
            elif stream.is_bts:
                urls = manifest.get_urls()
                print(json.dumps({
                    "status": "success",
                    "urls": urls,
                    "mime_type": stream.manifest_mime_type if stream.manifest_mime_type else "audio/mp4"
                }), flush=True)
            else:
                print(json.dumps({"status": "error", "error": "Unknown stream manifest format"}), flush=True)
        except Exception as e:
            handle_exception("Failed to resolve stream", e)
            sys.exit(1)

    else:
        print(json.dumps({"status": "error", "error": f"Unknown subcommand: {subcommand}"}), flush=True)
        sys.exit(1)

if __name__ == "__main__":
    main()
