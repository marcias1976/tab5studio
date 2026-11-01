# Uruchomienie Tab5 Studio na serwerze — instrukcja dla Claude Code

> **Dla użytkownika:** uruchom `claude` na serwerze i napisz:
> „Przeczytaj docs/SERVER-SETUP.md z repozytorium marcias1976/tab5studio i przeprowadź mnie przez uruchomienie.”
> Jeśli repozytorium nie jest jeszcze pobrane, napisz: „Sklonuj https://github.com/marcias1976/tab5studio do bieżącego katalogu, przeczytaj docs/SERVER-SETUP.md i przeprowadź mnie przez uruchomienie.”

---

**Dla Claude:** przeprowadź użytkownika przez uruchomienie Tab5 Studio w Dockerze na tym serwerze.

**Jak pracujesz:**
- Rozmawiaj po polsku, krótko i konkretnie.
- Wykonuj kroki po kolei. Po każdym kroku sprawdź wynik, zanim pójdziesz dalej.
- Najpierw przeczytaj `CLAUDE.md` w repozytorium: są tam zasady projektu.

**Zasady bezpieczeństwa:**
- Nigdy nie wypisuj haseł Wi-Fi ani PIN-u w odpowiedziach ani w logach.
- Pliku `.env` nie pokazuj w całości (`cat .env` jest zabronione). Jeśli trzeba coś w nim sprawdzić, sprawdzaj tylko nazwy zmiennych, np. `grep -o '^[A-Z_]*=' .env`.
- Przed czymkolwiek, czego nie da się cofnąć, zapytaj. Dotyczy to usuwania danych, zmian w zaporze, otwierania portów na świat i `docker system prune`.
- Nie wysyłaj nic do repozytorium (`git push`) bez wyraźnej prośby.

## 1. Sprawdź serwer

```bash
uname -m                      # x86_64 albo aarch64
docker --version && docker compose version
git --version
df -h .                       # potrzeba ok. 10 GB wolnego miejsca (obraz ESP-IDF ok. 4 GB + kompilacje)
free -h                       # zalecane 4 GB RAM lub więcej (kompilacja ESP-IDF)
```

- **Brak Dockera:** zaproponuj instalację według oficjalnej instrukcji dla systemu serwera (docs.docker.com/engine/install). Instaluj dopiero po zgodzie użytkownika.
- **Architektura aarch64 (ARM):** sprawdź, czy obraz `espressif/idf:v5.4.2` ma wersję na ARM (`docker manifest inspect espressif/idf:v5.4.2`). Jeśli nie ma, powiedz o tym użytkownikowi przed budowaniem.

## 2. Pobierz repozytorium

Repozytorium jest **publiczne**: `marcias1976/tab5studio`. Nie potrzeba `gh` ani logowania.

```bash
git clone https://github.com/marcias1976/tab5studio.git   # albo do bieżącego, pustego katalogu: git clone ... .
cd tab5studio
```

Do wysyłania zmian (`git push`) potrzebne jest konto z prawem zapisu, np. przez `gh auth login`. Logowanie wymaga przeglądarki i kodu, więc poproś użytkownika, żeby wpisał je sam przez `! gh auth login --web`.

Jeśli repozytorium już jest, zrób w nim tylko `git pull`.

## 3. Plik `.env`

```bash
cp .env.example .env
chmod 600 .env
```

Zapytaj użytkownika o wartości i wpisz je do `.env` (np. przez `sed`), nie powtarzając haseł w odpowiedzi:

| Zmienna | Co to jest |
|---|---|
| `WIFI_SSID`, `WIFI_PASSWORD` | sieć Wi-Fi, do której łączy się Tab5. Trafia do każdego skompilowanego programu. **Nazwa musi być dokładnie taka jak w routerze** (wielkość liter!), a sieć musi działać na 2,4 GHz: Tab5 nie obsługuje 5 GHz |
| `WEB_PIN` | 4-cyfrowy PIN strony www Tab5. Przy kompilacji w Studio można go zmienić |
| `STUDIO_PORT` | port Studio na serwerze, domyślnie 8080. **Sprawdź, czy jest wolny** (`ss -ltn \| grep :8080`); jeśli nie, zapytaj użytkownika o inny. W poleceniach poniżej zamień wtedy 8080 na ten port |
| `STUDIO_PASSWORD` | hasło do Studio. **Ustaw je, jeśli Studio będzie dostępne spoza sieci domowej** |
| `TAB5_HOST`, `TAB5_PIN` | opcjonalnie: adres i PIN Tab5 do podglądu na żywo (można je też ustawić w Studio) |

Najlepiej, żeby użytkownik wpisał wartości sam w osobnym terminalu (`nano .env`): hasła nie trafią wtedy do rozmowy.

Sprawdź tylko, które zmienne są ustawione, bez wartości:

```bash
grep -E '^[A-Z0-9_]+=' .env | sed -E 's/=(.+)$/=<ustawione>/; s/=$/=<puste>/'
```

Po każdej zmianie `.env` wystarczy restart kontenera (bez przebudowy): `docker compose up -d --force-recreate`.

## 4. Zbuduj i uruchom

```bash
docker compose up -d --build
docker compose ps
docker compose logs --tail 30 tab5studio
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:8080/   # 200 (albo 302 na /login, gdy jest STUDIO_PASSWORD)
```

Pierwsze budowanie pobiera obraz ESP-IDF (ok. 4 GB) i może trwać kilkanaście minut. Uprzedź o tym użytkownika.

Podaj użytkownikowi adres Studio: `http://<IP serwera>:<STUDIO_PORT>` (domyślnie 8080). IP sprawdzisz przez `hostname -I`.

## 5. Test kompilacji (zalecany)

Pierwsza kompilacja w kontenerze pobiera komponenty ESP-IDF, więc trwa kilka minut. Później ok. minuty.

Najprościej poproś użytkownika, żeby w Studio otworzył dolny panel → **Kompilacja** → **Kompiluj .bin**.

Możesz też uruchomić kompilację sam, przez API:

```bash
# pusty projekt z wbudowanymi ekranami
curl -s -X POST http://localhost:8080/api/build -H 'Content-Type: application/json' \
  -d '{"name":"test","lang":"pl","scene":{"colors":{"digit":"#5BE7E1","sensor":"auto"},"faces":{"basic":{"bg":"#050505","elements":[]},"full":{"bg":"#050505","elements":[]}}}}'
# → {"id":"..."}; potem co kilkanaście sekund:
curl -s "http://localhost:8080/api/build/<id>?offset=999999999" | python3 -c "import sys,json;print(json.load(sys.stdin)['status'])"
```

Gdy `STUDIO_PASSWORD` jest ustawione, dodaj do `curl` opcję `-u x:<hasło>`. Hasła nie wypisuj.

Wynik `done` oznacza sukces: pliki leżą w `data/builds/<id>/`. Przy `error` przeczytaj koniec logu (`curl -s http://localhost:8080/api/build/<id>`) i napraw przyczynę. Najczęstsze przyczyny:
- brak internetu w kontenerze (pobieranie komponentów);
- brak zmiennych `WIFI_*` lub `WEB_PIN` w `.env`;
- za mało RAM-u.

Testowy build możesz potem usunąć przez API (`DELETE /api/build/<id>`) albo w Studio.

### Pierwsze wgranie na Tab5

Powiedz użytkownikowi, że pierwszy raz wgrywa **pełny** `tab5panel-web-install.bin` (adres 0x0, np. ESP Web Tool w Chrome). Kasuje on ustawienia Tab5, ale Wi-Fi z `.env` jest w programie. Potem wygląd zmienia w Studio przez **Podgląd → Zapisz na Tab5 na stałe**, bez kompilacji. Kolejne aktualizacje programu wysyła się przez Wi-Fi przyciskiem **„Na Tab5”** przy kompilacji: ustawienia i zapisany projekt zostają.

## 6. Dane z Maca (opcjonalnie)

Projekty, biblioteka, obrazki i czcionki użytkownika są na jego Macu w `~/esp/tab5studio/data/`. Jeśli chce je mieć na serwerze, niech uruchomi na **Macu**:

```bash
rsync -av ~/esp/tab5studio/data/{projects,library,assets,fonts} <użytkownik>@<serwer>:<ścieżka>/tab5studio/data/
```

Po skopiowaniu sprawdź uprawnienia folderu `data/` (kontener pisze tam jako root) i odśwież Studio.

## 7. Dostęp i bezpieczeństwo

- **Podgląd na żywo** („Podgląd → Na Tab5”) wymaga, żeby serwer widział Tab5 w sieci lokalnej:
  - sprawdź to przez `curl -s -o /dev/null -w "%{http_code}\n" http://<IP Tab5>/` (oczekiwane 200);
  - adres i PIN Tab5 ustawia się w Studio: Podgląd → Połączenie z Tab5;
  - najlepiej ustawić Tab5 stały adres IP w routerze.
- **Port Studio (`STUDIO_PORT`, domyślnie 8080):** jeśli na serwerze działa zapora (`ufw status`), zapytaj, czy otworzyć port tylko dla sieci lokalnej, np. `ufw allow from 192.168.1.0/24 to any port 8080`.
- **Dostęp spoza domu:** tylko z `STUDIO_PASSWORD` i HTTPS, np. reverse proxy (Caddy, nginx, Traefik). Zaproponuj, ale nie konfiguruj bez zgody.
- **Kopie zapasowe:** wszystko, co ważne, jest w `data/`. Zaproponuj okresową kopię tego folderu.

## 8. Aktualizacja w przyszłości

```bash
cd tab5studio
git pull
docker compose up -d --build
```

## 9. Gdy coś nie działa

| Objaw | Co sprawdzić |
|---|---|
| Kontener się restartuje | `docker compose logs tab5studio`; brak `.env` albo zły format |
| Strona nie odpowiada z innego komputera | zapora, `docker compose ps` (port 0.0.0.0:8080), IP serwera |
| Tab5 nie łączy się z siecią z `.env` | dokładna nazwa sieci (wielkość liter), hasło, pasmo 2,4 GHz; czy sieć widać na liście „Dostępne sieci” na Tab5 |
| Kompilacja: brak `wifi_secrets.h` | brak `WIFI_SSID` w `.env`; po poprawce `docker compose up -d` |
| Kompilacja: błąd pobierania | internet i DNS w kontenerze: `docker compose exec tab5studio curl -sI https://components.espressif.com` |
| „Na Tab5”: brak połączenia | serwer nie widzi Tab5: inna sieć/VLAN, zły adres albo PIN |
| Brak miejsca | `docker system df`; stare kompilacje usuń w Studio. `docker image prune` tylko po zgodzie |

Po udanym uruchomieniu podsumuj użytkownikowi:
- adres Studio;
- czy test kompilacji przeszedł;
- czy podgląd na żywo widzi Tab5;
- co jeszcze zostało, np. hasło, HTTPS, kopie zapasowe.

Więcej o budowie projektu: `CLAUDE.md` i `docs/`.
