# Serveur HTTP et interface Web

Le module Web sert un dashboard statique embarqué, une API REST de supervision
et contrôle, ainsi que l'endpoint HTTP de métadonnées DPWS. Il utilise les
sockets Zephyr directement, sans serveur HTTP générique.

## Architecture

```mermaid
flowchart TD
    Browser["Navigateur IPv4 ou IPv6"] --> Listeners["Listeners TCP port 80"]
    DPWSClient["Client DPWS"] --> Listeners
    Listeners --> Poll["Thread app_web et poll"]
    Poll --> Parser["Parseur HTTP minimal"]
    Parser --> Assets["HTML, CSS, JS et logo en flash"]
    Parser --> REST["API REST JSON"]
    Parser --> Metadata["Endpoint SOAP DPWS"]
    REST --> Modbus["Holding registers Unit-ID 1"]
    REST --> Scanner["Fenêtre scanner Unit-ID 2"]
    REST --> Net["État réseau actif"]
    Metadata --> DPWS["app_dpws_build_metadata"]
    Browser -->|"polling frontend"| REST
```

## Contexte d'exécution

| Élément | Valeur |
|---|---|
| nom du thread | `app_web` |
| stack | 6144 octets |
| priorité | 8 |
| listeners | TCP IPv4 et IPv6, port 80 |
| backlog | 4 par listener |
| buffer de requête | 1536 octets, sur la stack du thread |
| buffer JSON | 2048 octets, statique et partagé séquentiellement |
| buffer metadata DPWS | 4096 octets, statique |

Le thread utilise `poll()` sur les deux listeners, accepte un client, lit une
requête, répond puis ferme la connexion. Il n'y a ni keep-alive ni traitement
concurrent : un client lent bloque temporairement les autres requêtes HTTP.

Le reboot Web est un work item différé de 500 ms exécuté par la workqueue
système, ce qui laisse le temps d'envoyer la réponse HTTP 202.

## Routes

### Assets

| Méthode | Route | Contenu |
|---|---|---|
| GET | `/` ou `/index.html` | dashboard HTML |
| GET | `/app.css` | styles |
| GET | `/app.js` | logique frontend |
| GET | `/logo.jpg` | logo binaire embarqué |

`app_web_assets.h` contient HTML/CSS/JS sous forme de chaînes C.
`app_web_logo.h` est la représentation C de `logo.jpg`. Une modification d'un
asset doit tenir compte de la consommation flash.

### API REST

| Méthode | Route | Effet |
|---|---|---|
| GET | `/api/status` | réseau actif, lien, connexions Modbus, heartbeat, statut |
| GET | `/api/registers` | les 60 holding registers |
| GET | `/api/registers/<id>` | un holding register |
| PUT | `/api/registers/<id>` | écrit une valeur 16 bits |
| GET | `/api/scanner` | mapping et valeurs des dix slots |
| GET | `/api/scanner/<id>` | un slot et son mapping |
| PUT | `/api/scanner/<id>` | écrit un slot scanner |
| POST | `/api/commands/save-config` | déclenche la sauvegarde réseau Modbus |
| POST | `/api/commands/reboot` | répond 202 puis redémarre |
| POST | `/dpws/<uuid>` | WS-Transfer Get pour les métadonnées DPWS |

Les écritures passent par les API Modbus/scanner afin de partager exactement
le même contrat et les mêmes validations que les protocoles industriels.

## Frontend

Le JavaScript interroge périodiquement l'API pour mettre à jour dashboard,
registres et scanner. Préserver les gardes `refreshInFlight` et les indicateurs
d'édition : sans elles, deux polls peuvent se chevaucher ou remplacer une
valeur que l'utilisateur est en train de saisir.

Le serveur écoute séparément en IPv4 et IPv6 avec `IPV6_V6ONLY`. Pour une IPv6
link-local, le client doit préciser l'interface, par exemple avec curl Windows :

```text
curl.exe -g "http://[fe80::80:e1ff:fe4c:b2d7%2511]/"
```

Dans une URL, `%25` encode le caractère `%` qui introduit l'index de zone.

## Limites et sécurité

- HTTP clair, sans authentification ni autorisation ;
- parseur limité à une requête contenue dans 1536 octets ;
- aucune prise en charge de chunked encoding, keep-alive ou pipeline ;
- parsing JSON minimal : seule une valeur entière 16 bits est recherchée ;
- les routes PUT, SAVE et reboot sont des interfaces de contrôle non protégées.

Toute évolution de sécurité doit protéger toutes les routes mutables, pas
seulement l'interface HTML.

## Ajouter une route

1. Définir le contrat JSON ou SOAP et ses bornes.
2. Router dans `handle_api_get`, `handle_api_put`, `handle_api_post` ou
   `handle_http_request`.
3. Utiliser l'API du module propriétaire des données.
4. Vérifier tous les retours de `snprintk` et les tailles de buffers.
5. Mettre à jour ce tableau et tester IPv4 plus IPv6.

## Validation

Tester les assets, toutes les routes, les identifiants hors bornes, une requête
malformée et les écritures supérieures à 65535. Garder simultanément une
connexion Modbus idle et une connexion EIP Class 1 pour détecter une régression
de capacité sockets.
