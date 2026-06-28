# dd — code signing & notarization (macOS)

Developer ID signing material for the `dd` GUI app. **Everything here except `README.md` and
`.gitignore` is gitignored** (private keys, certs, the `.p12`, API keys, and the GitHub Actions secret
values). Never commit those.

## How a release gets signed

Push a tag `vX.Y.Z` → `.github/workflows/release.yml` runs on a macOS runner and:
1. imports the Developer ID cert from the `DD_SIGN_P12_BASE64` / `DD_SIGN_P12_PW` secrets into a temp keychain, plus Apple's G2 intermediate;
2. builds `dd.app` + `dd.dmg` (`make dmg`), where `dd-gui/package/bundle.sh` signs every Mach-O with **Developer ID Application: Richard Hutta (SMS68QA6VC)** + hardened runtime + secure timestamp (+ the `allow-jit` entitlement on the JIT engines);
3. notarizes the DMG with the app-specific password (`APPLE_ID` / `APPLE_TEAM_ID` / `APPLE_APP_PW`) and staples it;
4. publishes the signed + notarized DMG to the GitHub Release.

GitHub runners have a correct clock, so notarization works there. (Locally on the orbstack mac the clock is hours-skewed → notarytool's S3 upload fails with `RequestTimeTooSkewed`; sign locally, notarize in CI.)

## GitHub Actions secrets (5) — values are in `github-secrets.txt` (gitignored)

Add each at **Settings → Secrets and variables → Actions → New repository secret**:

| Secret | What |
|---|---|
| `DD_SIGN_P12_BASE64` | base64 of `developerID.p12` (cert + private key) |
| `DD_SIGN_P12_PW`     | the `.p12` password |
| `APPLE_ID`           | Apple ID email for notarization |
| `APPLE_TEAM_ID`      | `SMS68QA6VC` |
| `APPLE_APP_PW`       | app-specific password (appleid.apple.com) |

Then `git tag vX.Y.Z && git push origin vX.Y.Z` → a signed + notarized release.

## Files here (all gitignored)

- `developerID.p12` — cert + key bundle (source of `DD_SIGN_P12_BASE64`; password in `github-secrets.txt`).
- `developerID.{key,csr,pem}` + `developerID_application.cer` — private key, the CSR uploaded to Apple, and the issued cert.
- `AuthKey_*.p8` — App Store Connect API keys (they 401; the app-specific password is used instead).
- `asc.py` — App Store Connect API helper (`python3 asc.py list`).
- `github-secrets.txt` — the 5 secret values, ready to paste into GitHub.

## Local signing

`make app DD_SIGN_ID="Developer ID Application: Richard Hutta (SMS68QA6VC)" DD_SIGN_KEYCHAIN=<kc> DD_SIGN_KEYCHAIN_PW=<pw>`
signs `dd.app`; `make dmg` also notarizes + staples when `DD_NOTARY_PROFILE` (or inline `DD_NOTARY_APPLE_ID`/`DD_NOTARY_TEAM_ID`/`DD_NOTARY_PW`) is set — but the host clock must be synced.
