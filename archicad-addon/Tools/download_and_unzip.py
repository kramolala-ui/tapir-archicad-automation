import os
import sys
import subprocess
import platform
import zipfile
import time
import urllib.request
import urllib.error
import urllib.parse

url = sys.argv[1]
dest = sys.argv[2]

if os.path.exists (dest):
    sys.exit (0)

os.makedirs (dest)

fileName = os.path.split (urllib.parse.unquote (url).split ('?')[0])[1]
filePath = os.path.join (dest, fileName)


def download_with_retry (url, file_path, max_attempts = 5, timeout = 120):
    """Скачать файл с retry и exponential backoff.

    GitHub releases иногда отвечает медленно / рвёт соединение
    (WinError 10060 при timeout коннекта). Один urlretrieve без
    повторов валит CI на транзиентных сбоях сети — 5 попыток с
    backoff закрывают эту проблему.
    """
    last_err = None
    for attempt in range (1, max_attempts + 1):
        print (f'[download] attempt {attempt}/{max_attempts}: {url}', flush = True)
        try:
            req = urllib.request.Request (
                url, headers = {'User-Agent': 'tapir-archicad-automation-ci'}
            )
            with urllib.request.urlopen (req, timeout = timeout) as resp, \
                 open (file_path, 'wb') as out:
                while True:
                    chunk = resp.read (1024 * 1024)   # 1 MB
                    if not chunk:
                        break
                    out.write (chunk)
            size = os.path.getsize (file_path)
            print (f'[download] OK: {size} bytes', flush = True)
            return
        except Exception as e:
            last_err = e
            print (
                f'[download] attempt {attempt} failed: '
                f'{type (e).__name__}: {e}',
                flush = True
            )
            # Частично скачанный файл удаляем, чтобы следующий attempt
            # начинал с нуля и не оставил мусор для zipfile.
            try:
                if os.path.exists (file_path):
                    os.remove (file_path)
            except OSError:
                pass
            if attempt < max_attempts:
                delay = min (30, 2 ** attempt)   # 2s, 4s, 8s, 16s, max 30
                print (f'[download] retry in {delay}s', flush = True)
                time.sleep (delay)
    raise last_err


download_with_retry (url, filePath)

if platform.system () == 'Windows':
    with zipfile.ZipFile (filePath, 'r') as zip:
        zip.extractall (dest)
elif platform.system () == 'Darwin':
    subprocess.call ([
        'unzip', '-qq', filePath,
        '-d', dest
    ])

sys.exit (0)
