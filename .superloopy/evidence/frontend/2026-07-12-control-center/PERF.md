# Measured quality evidence

## Design-system compliance

PASS. The Superloopy compliance script reported zero undeclared colors and zero off-scale spacing violations across the token CSS, application CSS, shell, and three page components.

## React Doctor

PASS with React Doctor 0.7.5. It scanned nine source files and reported zero errors and zero warnings after the pnpm supply-chain policy and unused type export were corrected.

## Lighthouse

Production Vite output was measured through a separately launched full Chrome 149 remote-debugging session, not Chrome Headless Shell. Three mobile and three desktop runs were recorded as JSON under `lighthouse/`.

Post-asset median before the final robots correction:

| Mode | Performance | Accessibility | Best practices | SEO |
| --- | ---: | ---: | ---: | ---: |
| Mobile | 96 | 100 | 81 | 66 |
| Desktop | 100 | 100 | 81 | 66 |

The crawl policy was corrected and a targeted final mobile run produced performance 96, accessibility 100, best practices 77, and SEO 100.

The remaining best-practices failure is external to the application. The report identifies one insecure request injected by the locally installed AdGuard service at `http://local.adguard.org/`, with `app=chrome.exe`. The application itself requests no insecure resource. GitHub Actions therefore runs the same Lighthouse 13.4.0 audit on a clean Ubuntu runner and enforces performance 90+, accessibility 100, best practices 90+, and SEO 90+ as a merge-blocking gate.

No UX or content was removed to improve these scores.
