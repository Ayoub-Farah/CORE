# Rapport de conception — OTA CAN minimal sur les boards

Date : 25 septembre 2026. Référence du dépôt : `5930ee6f4b2058437ea01bb1e4bf62376e541935`, branche `feat/thingset-can-ota`.

**Statut : spécification de référence. L'implémentation logicielle v2 est disponible sur `feat/minimal-can-ota` ; voir le [bilan des tests et des mesures](../docs/minimal-can-ota-validation.md). Ce document conserve les mesures du prototype et les objectifs de conception. La qualification matérielle, la marge RAM dynamique et l'adaptateur MMC sûr restent à valider ; MMC_ANA est inchangé.**

## 1. Objectif et décisions

Réduire en priorité la Flash, la RAM et l'activité en arrière-plan de l'OTA sur les boards qui exécutent MMC_ANA. La consommation du Lead est secondaire ; il reste néanmoins soumis aux limites physiques de son matériel.

Le système doit diffuser la même image à toutes les boards sélectionnées sur un bus CAN commun. Le fonctionnement de puissance est mis en pause pendant la mise à jour. Hors mise à jour, l'OTA doit perturber le moins possible le contrôle HRTIM et les échanges RS485.

Décisions pour cette évolution :

1. **Deux variantes compilées : un récepteur léger et un Lead dédié.** Toutes les boards réceptrices utilisent la même image ; le Lead dispose d'une autre image et ne reçoit pas le firmware MMC destiné aux boards.
2. **MMC_ANA reste inchangé.** Ne pas réduire ses canaux Scope, sa profondeur d'acquisition, ses algorithmes, sa synchronisation ou son protocole RS485. Les adaptations se placent dans Core, le module OTA, ses profils de compilation et les outils PC.
3. **Initialisation individuelle par USB, sans CAN ni ST-Link**, pour les boards et le Lead. Conserver une récupération USB avec le bootloader existant, sous réserve qu'il soit intact et compatible. BOOT + RESET peut rester nécessaire pour une récupération manuelle.
4. **Conserver d'abord ThingSet/CAN en mode binaire.** Retirer les capacités inutiles au récepteur avant d'envisager un autre protocole.
5. **Ne pas transférer de marqueur d'activation avec les données CAN.** Valider l'image complète, puis armer explicitement son essai après la décision collective de commit.
6. **Conserver les contrôles locaux et le retour à l'ancienne image.** Les gains doivent venir des fonctions Lead, des duplications et du dimensionnement des buffers, pas de la suppression des garanties de reprise.

Ce rapport complète le [rapport initial](thingset_can_ota_implementation_report.md). Pour la nouvelle variante minimale, il remplace deux choix du prototype : même binaire sur le Lead et les boards, et transfert d'un fichier paddé contenant déjà un trailer activable. Les [documents d'implémentation actuels](../docs/ota-implementation.md) continuent de décrire le logiciel livré, jusqu'à sa migration explicite.

## 2. Mesures et budget mémoire

### 2.1. Méthode et limites

Les mesures proviennent d'une compilation isolée de Core avec le `src/main.cpp` de `C:/Users/afarahhass/Documents/Repo/MMC/MMC_ANA`, sans modification du fichier source. Cette compilation de dimensionnement ne constitue pas une intégration opérationnelle : les adaptations de maintenance, de santé et de temps réel ne sont pas qualifiées.

SHA-256 du `main.cpp` original et de sa copie de mesure :

```text
36B01F28C82D51DEA740A62825ECB60CC4F7AAB0753118B8B2077E18C2E9938D
```

Preuves locales, ignorées par Git et à archiver avec les futurs résultats :

- `.pio/mmc-size-build.log` : compilation MMC avec USB et essai avec OTA actuelle.
- `.pio/mmc-size-baseline-ota.map` : carte mémoire du lien OTA ayant échoué.
- `.pio/ota-footprint-analysis.json` : attribution des sections aux objets compilés.
- `.pio/mmc-size-binary-thingset.log` : essai ThingSet binaire uniquement.
- `.pio/mmc-size-check/SIZING_ONLY.txt` : limites de l'expérience.

Les valeurs ci-dessous utilisent les réservations du linker, et non seulement le résumé PlatformIO, qui ne compte pas toutes les zones de la même façon. Une section initialisée peut occuper de la Flash pour sa valeur initiale et de la RAM pendant l'exécution. Les allocations dynamiques sont ajoutées séparément.

### 2.2. Capacité utile

| Zone | Taille | Rôle |
|---|---:|---|
| Flash physique | 524 288 octets | 512 Kio au total |
| Bootloader | 65 536 octets | Conservé |
| Slot primaire | 227 328 octets | Application active |
| Slot secondaire | 227 328 octets | Candidate, puis ancienne image selon le swap |
| NVS | 4 096 octets | Données persistantes partagées avec Core |
| Borne utile de l'image signée | **221 184 octets** | Borne actuellement configurée, à qualifier avec le bootloader et l'armement différé |
| RAM physique | 131 072 octets | 128 Kio |
| RAM utilisable par le lien mesuré | **130 048 octets** | 127 Kio |

Le partitionnement est défini dans [spin.dts](../zephyr/boards/owntech/spin/spin.dts). Les 227 328 octets du slot ne sont pas une autorisation de produire autant de code utile : header, signature, TLV, trailer et contraintes du swap doivent être pris en compte. Ne pas gagner de place en supprimant le slot de retour arrière.

### 2.3. Comparaison demandée

Dans les tableaux arrondis, **1 Ko = 1 000 octets**. « OTA minimale » désigne un budget de conception, pas un résultat ni un minimum garanti.

| Ressource | MMC seul, avec USB | Ajout OTA actuel | Ajout OTA minimal visé | Capacité disponible | Reste après MMC seul |
|---|---:|---:|---:|---:|---:|
| Flash au lien, avant finalisation de la signature | 131,5 Ko | 113,0 Ko | **≤83,0 Ko** | **221,2 Ko pour l'image signée** | 89,7 Ko avant métadonnées finales |
| RAM connue, Scope inclus | 93,3 Ko | 64,8 Ko | **25 à 28 Ko** | **130,0 Ko** | 36,8 Ko avant autres allocations |

| Configuration | Flash au lien | RAM réservée au lien | Allocation Scope connue | RAM connue totale |
|---|---:|---:|---:|---:|
| MMC + USB | 131 496 | 35 584 | 57 680 | **93 264** |
| MMC + OTA actuelle | 244 536 | 100 388 | 57 680 | **158 068** |
| MMC + récepteur minimal, plafond de travail | 214 496 | 63 584 | 57 680 | **121 264** |

Toutes les valeurs de ce second tableau sont en octets. La dernière ligne suppose un ajout OTA de 83 000 octets Flash et 28 000 octets RAM ; elle n'est pas mesurée.

L'OTA actuelle ajoute exactement **113 040 octets de Flash et 64 804 octets de RAM réservée**. Avec MMC, le lien échoue déjà de 17 208 octets par rapport au slot de 227 328 octets. Pour respecter la borne utile et la signature, l'économie minimale nécessaire est plus proche de **24 Ko**, avant toute réserve. Le déficit RAM connu est de **28 020 octets**, avant les autres allocations dynamiques.

Le plafond de travail vise donc environ **30 Ko de Flash et 37 Ko de RAM économisés sur l'OTA**, soit 27 % et 57 % de son surcoût actuel. Le récepteur seul peut permettre davantage d'économies ; il faut le mesurer avant d'annoncer un meilleur résultat.

### 2.4. Scope et réserve réelle de MMC

MMC crée `ScopeMimicry scope(1028, 14)` :

```text
Mesures :             1028 × 14 × 4 = 57 568 octets
Deux tableaux :        14 × 4 × 2 =    112 octets
Allocation connue totale :          57 680 octets
```

Cette allocation a lieu à la construction de l'objet global, y compris sur une board qui ne connecte ensuite aucun canal Scope. Elle n'apparaît pas comme tableau statique dans le résumé PlatformIO. La pause de MMC ne la libère pas. Les frais de l'allocateur et les autres allocations, notamment celles de DataAPI, restent à mesurer.

Avec un récepteur de 28 000 octets de RAM supplémentaire, il reste **8 784 octets** après Scope. À 25 000 octets, il reste **11 784 octets**. Ce n'est pas encore la réserve finale de fonctionnement.

Pour réserver réellement 8 192 octets après toutes les allocations, appliquer :

```text
Budget RAM OTA maximal = 130048 - 93264 - autres_allocations - 8192
                       = 28592 - autres_allocations
```

Par exemple, 4 096 octets supplémentaires d'allocations et de frais mémoire ramèneraient ce budget OTA à **24 496 octets**. Les piles déjà réservées au lien ne doivent pas être comptées une seconde fois.

En Flash, la cible laisse 6 688 octets sous la borne utile avant finalisation de la signature, soit environ 6,4 Ko avec l'ordre de grandeur des métadonnées de l'image actuelle. Seul le fichier signé final permet de calculer la marge exacte.

## 3. Pourquoi le récepteur actuel est trop lourd

Le [CMake OTA](../zephyr/modules/owntech_ota/zephyr/CMakeLists.txt) compile toujours `ota_coordinator.cpp` et `ota_runtime.cpp`. `CONFIG_OWNTECH_OTA_LEAD` ne conditionne actuellement que `ota_usb.cpp`. Les tableaux et le coordinateur restent déclarés dans [ota_runtime.cpp](../zephyr/modules/owntech_ota/zephyr/src/ota_runtime.cpp), même pour une board non-Lead.

| Principal poste de RAM supplémentaire | Taille approximative | Origine |
|---|---:|---|
| Runtime OTA | 28,5 Ko | Dont environ 14,1 Ko de tables/coordinateur, 8,3 Ko de pile et 4,2 Ko de file |
| ThingSet et transport CAN | 19,1 Ko | Buffers, contextes et tâches |
| SMP/MCUmgr et transport série | 9,7 Ko | Buffers et pile de traitement |
| CDC USB supplémentaire | 2,4 Ko | Interface OTA applicative |
| Signalisation LED | 1,1 Ko | Tâche dédiée et pile |
| Autres contributions | Environ 4 Ko | Stockage, états, noyau et alignement |

Cette attribution est approximative ; certaines dépendances sont partagées et les gains de suppression ne sont pas nécessairement additifs.

En Flash, le surcoût vient principalement du runtime et du stockage OTA, des transports CAN/USB, de ThingSet et des conversions texte/nombres. La fonction SHA-256 liée représente de l'ordre de 1 Ko dans l'analyse : supprimer les contrôles d'intégrité n'est pas le bon levier.

**Gain déjà mesuré, uniquement dans la copie de dimensionnement :** `CONFIG_THINGSET_TEXT_MODE=n` abaisse la Flash de 244 536 à 232 164 octets, soit **12 372 octets économisés**, sans modifier les 100 388 octets de RAM réservée. La compilation reste trop grosse. Aucun fonctionnement matériel n'a été validé avec cette variante.

## 4. Architecture cible

```text
PC : construit et conserve l'image des boards, conserve le journal de campagne
  │ USB : commandes, crédits de transfert et blocs bornés
  ▼
Lead dédié : inventaire, coordination, diffusion et contrôle des résultats
  │ CAN FDCAN2, 500 kbit/s, diffusion commune et réponses adressées
  ├── Board 1 : MMC inchangé + récepteur OTA minimal
  ├── Board 2 : même image
  └── Board N : même image
```

### 4.1. Répartition des responsabilités

| Fonction | PC / Lead | Chaque board |
|---|---|---|
| Liste complète et figée des participants | Oui | Non |
| Historique et diagnostic détaillé de campagne | Oui | Non |
| Source complète du firmware | Fichier conservé sur le PC | Slot secondaire local |
| Décision de rediffusion | Lead | Signale son offset accepté et ses erreurs |
| Gestion USB de la campagne | Lead | Non |
| Réception de commandes et blocs CAN | Émission/coordination | Oui |
| Hash, bornes, compatibilité et fermeture du writer | Contrôle des réponses | Vérification locale obligatoire |
| État persistant | Campagne collective | État local minimal et identité du coordinateur |
| Maintenance, essai et confirmation de l'image | Suivi collectif | Exécution locale obligatoire |

La mémoire propre au récepteur doit être essentiellement **indépendante du nombre de boards**. Il conserve son état et l'identité du Lead autorisé pour la campagne, pas les observations de tous ses voisins. Les mécanismes d'adressage CAN et de détection de conflits restent présents dans la mesure requise par le transport.

### 4.2. Séparer réellement les compilations

Extraire du runtime actuel les fonctions exclusivement Lead : inventaire, liste figée, coordinateur, découverte de flotte, staging USB, réconciliation collective et émission du fichier. Les compiler seulement dans la variante Lead.

Le runtime récepteur garde une seule instance de participant, son writer, un état de santé, un journal local, une file bornée et l'adaptateur CAN. Séparer l'API commune des API Lead pour éviter que des références communes forcent le linker à conserver le coordinateur.

Noms proposés, non encore disponibles : `ota_receiver_runtime.cpp`, `ota_lead_runtime.cpp`, `ota_receiver.conf` et `ota_lead.conf`. Le choix des noms peut évoluer ; la séparation vérifiée dans les fichiers `.map` est le critère de réussite.

Sur les boards, retirer le serveur applicatif USB/SMP réservé à l'OTA et sa seconde interface CDC. **Conserver l'USB et les fonctions de console/Scope utilisées par MMC.** Ne pas désactiver globalement une bibliothèque libc ou un formatage nécessaire à MMC pour obtenir artificiellement un gain.

### 4.3. Source de l'image et rôle du Lead

Le Lead dédié ne doit jamais adopter comme future application le firmware destiné aux boards. Ne pas reprendre tel quel le staging actuel dans son slot secondaire : le fichier paddé peut l'armer et transformer le Lead en simple récepteur au prochain reset.

Choix recommandé pour cette évolution : le PC conserve le fichier complet et sert les blocs demandés par le Lead. Le Lead garde une fenêtre bornée et diffuse les blocs sur CAN. Les demandes de rediffusion relisent le fichier PC. Il n'est donc pas nécessaire de garder une image entière en RAM ni de remplacer l'application du Lead.

Le protocole USB doit prévoir un contrôle de débit, les offsets, la campagne, le hash du fichier, des délais bornés et des reprises explicites. Une interruption du PC interrompt la progression et conserve la maintenance ; elle ne devient jamais un commit implicite. Une source Flash alternative sur le Lead serait un travail distinct, avec preuve qu'elle est non activable.

Le Lead est exclu de la liste des cibles MMC et du comptage attendu. Son propre firmware s'initialise et se met à jour séparément par USB. Son identité matérielle ne suffit plus à distinguer les images : ajouter une **classe d'image/capacité `receiver` ou `lead`** au manifeste et aux vérifications de compatibilité. Le bootloader actuel ne connaît pas nécessairement cette classe ; les outils et les services doivent la vérifier avant écriture.

## 5. Réduction des ressources du récepteur

Ordre recommandé :

1. **Retirer les fonctions Lead à la compilation.** Vérifier l'absence du coordinateur et des tableaux `inventory`/`frozen` dans le binaire récepteur. Une petite donnée locale peut rester nécessaire pour lier l'identité et l'adresse du Lead.
2. **Retirer SMP/MCUmgr applicatif et le CDC OTA des boards.** Adapter d'abord la vérification USB d'initialisation ; ne pas laisser le script chercher indéfiniment un serveur supprimé.
3. **Passer ThingSet en binaire uniquement.** Conserver le format et les fonctions utilisés par le contrôle CAN ; tester l'interopérabilité avec le Lead.
4. **Éliminer les fonctions clientes ThingSet propres au Lead.** Garder l'adressage, les réponses, l'émission des statuts et les mécanismes de transport dont le participant dépend réellement.
5. **Dimensionner les buffers pour une seule campagne.** Un nombre de boards élevé n'implique pas autant de buffers simultanés dans chaque board. Fixer la taille maximale des commandes, statuts et blocs avant de réduire les pools.
6. **Compacter la file de travail.** Les variantes commande/données peuvent partager une union ; réduire la profondeur après essais de pertes et de débit. Une file pleine doit produire un échec ou une rediffusion explicite, jamais un succès silencieux.
7. **Mesurer puis réduire les piles.** Utiliser les points hauts en réception, effacement, vérification, erreur et reprise. Le coût du flash et du hash ne doit pas se déplacer dans une interruption CAN ni une workqueue critique.
8. **Supprimer le réveil LED permanent.** La tâche actuelle se réveille toutes les 25 ms. Préférer une signalisation sur événements et un timer actif uniquement lorsque nécessaire, en préservant les appels LED de l'application, y compris depuis les interruptions.

Le tableau `coordinator.observations` représente à lui seul environ 4 096 octets ; la fusion de copies et la suppression du coordinateur se recouvrent. Ne pas additionner ces économies comme si elles étaient indépendantes. De même, retirer une fonctionnalité ne supprime pas forcément une dépendance encore utilisée ailleurs : mesurer les différences du linker après chaque étape.

Une réécriture en CAN brut est une option de second rang, uniquement si la séparation et le dimensionnement échouent au budget. Elle devrait reconstruire fragmentation, identité, intégrité, contrôle de débit, perte et reprise ; aucun gain ni équivalence de sûreté n'est acquis d'avance.

## 6. Contrat de mise à jour et sûreté

### 6.1. Image compacte et activation différée

Le prototype actuel transfère un trailer activable et suppose l'absence de reset ou de coupure entre staging et redémarrage final. Cette hypothèse ne satisfait pas l'objectif de résistance aux coupures de la variante décrite ici.

Produire un artefact CAN signé compact avec la chaîne et la clé compatibles existantes, sans trailer d'activation. **Ne pas tronquer arbitrairement `firmware.mcuboot.bin`.** Adapter le générateur, le manifeste, les limites du writer et les tests d'artefacts. Un artefact d'installation USB distinct peut rester nécessaire pour le service du bootloader ; le qualifier séparément et le distinguer explicitement dans les outils.

Conserver deux domaines de hash : celui des octets exacts transférés et le hash MCUboot de l'image exécutée. Ajouter une version explicite du nouveau contrat d'artefact/protocole. Refuser un ancien manifeste paddé dans un récepteur minimal qui attend une image non armée.

Avant d'accepter les blocs, effacer/préparer le slot secondaire sous maintenance et vérifier qu'aucun ancien marqueur d'activation ne subsiste. Les détails de l'effacement et de l'armement doivent être qualifiés avec la géométrie et l'algorithme du bootloader installé. Si cette preuve échoue, la garantie d'activation différée reste bloquée : ne pas revenir silencieusement à l'ancien comportement.

Avant cet effacement, exiger une image active confirmée, aucun swap/essai/retour arrière en attente et aucun autre propriétaire du slot. Refuser `PREPARE` si le slot conserve une image encore nécessaire au rollback. Lire l'état réel du bootloader ; un journal OTA au repos ne suffit pas à établir que le slot est libre.

Préserver physiquement effacés les emplacements du trailer qui seront programmés lors de l'armement ou de la confirmation. Ne pas y écrire du padding `FF` : une relecture à `FF` ne prouve pas que les bits ECC sont encore effacés. Ce piège et les échecs de confirmation associés sont documentés dans la [récupération STM32G4](../docs/ota-recovery.md#sparse-primary-programming-and-flash-ecc). Le nouveau générateur et le writer doivent être testés ensemble sur ce point.

### 6.2. Séquence d'une campagne

Les noms ci-dessous désignent la séquence cible, pas de nouvelles commandes déjà livrées.

1. Le PC vérifie l'artefact, sa classe `receiver`, son identité et ses tailles. Le Lead fige explicitement la liste des EUI attendus, sans s'y inclure.
2. Chaque board accepte `PREPARE` seulement après compatibilité, disponibilité du stockage, mise en maintenance et persistance relue de cette maintenance. Le Lead attend toutes les réponses avant les données.
3. Le Lead diffuse des blocs bornés. Chaque board vérifie campagne, source, offset, longueur et CRC avant d'écrire séquentiellement. Une perte arrête la progression contiguë jusqu'à la réparation ; une panne Flash est une erreur fatale.
4. Après flush et fermeture du writer, chaque board relit l'image complète, vérifie sa structure, ses tailles et ses hashes, puis publie `VALID`. Réception complète, image valide et image exécutée sont trois états différents.
5. Le Lead ne décide `COMMIT` que si toutes les identités figées sont `VALID` pour la même image. Il ne retire jamais silencieusement une carte absente.
6. Chaque board persiste l'intention de commit, arme l'essai MCUboot par le mécanisme compatible, puis acquitte un résultat durable. Prévoir explicitement les coupures entre ces écritures ; une réponse positive ne doit pas précéder les garanties qu'elle annonce.
7. Le redémarrage est demandé après les acquittements d'armement. Une coupure peut néanmoins faire redémarrer certaines boards avant les autres : maintenir l'inhibition locale au démarrage.
8. Chaque board vérifie sa santé et l'image réellement démarrée avant confirmation. Le Lead/PC vérifie ensuite l'ensemble de la flotte avant la libération collective de maintenance, liée à la campagne et au hash attendus.

« Simultané » signifie ici diffusion commune puis barrière collective de validation. **Le CAN ne garantit ni un reset exactement simultané, ni une transition atomique de toutes les cartes.** Une coupure pendant l'armement peut laisser une flotte mixte ; l'état partiel doit rester visible et la puissance inhibée jusqu'à réconciliation.

### 6.3. Journal local et récupération

Conserver seulement les informations locales nécessaires : version de format, campagne, identité du coordinateur, image attendue, phase durable, intention/résultat de commit et état de maintenance. Les listes et historiques de flotte restent sur Lead/PC. Utiliser le propriétaire NVS existant de Core et conserver calibration, seuils et métadonnées.

Avant tout effacement d'image, vérifier que NVS peut écrire et remplacer les enregistrements requis. Budgéter la coexistence temporaire des anciens et nouveaux journaux, les métadonnées, le ramasse-miettes et les autres données Core : la taille finale du journal ne suffit pas. Relire les écritures critiques et refuser la campagne si cette réserve n'est pas disponible.

Ne pas reprendre une écriture partielle après reset dans la première version : rester en maintenance et recommencer proprement après diagnostic. La répétition d'une commande compatible doit être idempotente ; une commande contradictoire doit être refusée. Une corruption de journal bloque la reprise normale et déclenche une récupération explicite, sans effacement global automatique de NVS.

Définir une migration versionnée des journaux de l'ancien prototype. Une trace de campagne ancienne ne doit pas être confondue avec la campagne courante. `ABORT` avant armement conserve une image non activable ; après armement, ne pas prétendre désarmer sans lecture et traitement explicites de l'état du bootloader.

Conserver la signature MCUboot existante. Les hashes et CRC ne sont pas une authentification des commandes CAN. Le périmètre reste celui d'un bus de confiance ; une protection contre des commandes malveillantes nécessiterait un travail supplémentaire.

## 7. Initialisation et récupération USB sans ST-Link

Le retrait de SMP dans l'application réceptrice ne retire pas le service USB du bootloader. En revanche, [provision_ota.py](../owntech/tools/provision_ota.py) doit distinguer un récepteur minimal sain d'une application absente ou incompatible.

Parcours cible :

1. Brancher une seule carte par USB, CAN déconnecté. Sélectionner son numéro de série stable ; aucune ambiguïté ne doit provoquer une écriture.
2. Entrer dans le bootloader par le mécanisme logiciel déjà compatible ou, si nécessaire, BOOT + RESET. Lire l'état des images avant d'écrire.
3. Installer l'image de la bonne classe par la voie USB qualifiée, sans remplacer le bootloader ni la clé.
4. Au premier démarrage sans campagne en attente, valider stockage, contrôleur CAN et santé locale, puis confirmer l'image sans attendre un autre nœud CAN actif. L'attente d'un ACK CAN ne doit pas faire échouer une carte seule.
5. Vérifier par USB l'identité, le build, le hash actif, la confirmation et l'état local. Implémenter pour cela un statut compact en lecture seule, borné et à la demande sur l'USB existant, sans réintroduire le serveur SMP complet ni perturber Scope. Son coût entre dans le budget récepteur.

L'accès à 1200 bauds et les requêtes de statut doivent préserver la mise en sécurité et éviter le blocage historique de la console USB. Ne jamais injecter aveuglément une sonde SMP dans une console ancienne qui ne la consomme pas. Détecter explicitement les états legacy, ancien OTA et nouveau récepteur minimal.

Le bootloader OwnTech déjà étudié n'expose pas nécessairement le même parcours dans tous les états d'image. Une liste vide, une image d'essai non confirmée ou un refus `EBADSTATE` ne sont pas un feu vert pour forcer l'upload. Conserver les contrôles et les diagnostics de [bootloader_upload.py](../owntech/tools/bootloader_upload.py) et de la [récupération actuelle](../docs/ota-recovery.md).

Une nouvelle installation, deux mises à jour consécutives et une récupération après interruption doivent être démontrées sans ST-Link. La promesse ne couvre pas un bootloader effacé ou un défaut matériel.

## 8. MMC inchangé et comportement hors campagne

Intégrer dans Core/OTA un adaptateur pour les callbacks `owntech_ota_enter_maintenance()` et `owntech_ota_check_health()`. Les valeurs faibles actuelles refusent volontairement ; ne pas les remplacer par un succès inconditionnel.

L'adaptateur doit prouver l'arrêt des sorties, empêcher un redémarrage par RS485 ou une autre tâche, maintenir les protections indispensables et vérifier l'initialisation réelle de MMC avant confirmation. Inspecter toutes les voies de commande de puissance, y compris les accès matériels directs. Si ces garanties ne peuvent pas être obtenues sans modifier MMC, consigner précisément le point bloquant ; ne pas déclarer l'intégration sûre sur la seule réussite du lien.

Brochage à conserver : **FDCAN2 PB5 RX / PB6 TX**, synchronisation **HRTIM PB2 SCIN / PB1 SCOUT**. Garder le CAN classique à 500 kbit/s pour cette qualification.

Hors campagne : pas de diffusion périodique de métriques OTA, pas de scrutation de flotte sur les boards, worker en attente d'événements et timers seulement lorsqu'un état l'exige. Utiliser les filtres CAN adaptés et des traitements bornés ; conserver les priorités qui permettent au contrôle et au RS485 de préempter le travail OTA.

Ne pas promettre un coût CPU nul : les interruptions, l'adressage CAN et les requêtes reçues existent encore. Mesurer latence et gigue HRTIM, délai et pertes RS485 avec CAN désactivé, activé silencieux, puis sous découverte/statuts et trafic perturbateur. Les écritures Flash appartiennent à la phase de maintenance, pas à l'essai de contrôle temps réel normal.

## 9. Environnements et fichiers à faire évoluer

Conserver des noms génériques, sans blink A/B ni rôle matériel A/B. Organisation proposée :

| Environnement / tâche | Comportement cible |
|---|---|
| `OTA` | Compile `src/main.cpp` avec le récepteur minimal ; initialise une board seule par USB |
| `USB_LEAD` — installation | Compile et installe le firmware dédié au Lead, séparé de `src/main.cpp` utilisateur |
| `USB_LEAD` / `lead_update` | Construit explicitement l'artefact récepteur `OTA`, puis le diffuse aux boards ; ne flashe pas le Lead |
| `USB` | Workflow applicatif existant, sans promesse de conserver le récepteur OTA |
| `OTA_RECOVERY` | Utilitaire de réparation explicite, adapté au nouveau journal si nécessaire |

Renommer le libellé « Update Lead and CAN fleet » en un libellé indiquant clairement la mise à jour des boards. `Upload and Monitor` sert à l'installation/observation de la cible USB, pas à la campagne CAN.

| Sources existantes | Travail principal |
|---|---|
| [owntech/pio_extra.ini](../owntech/pio_extra.ini), [zephyr/CMakeLists.txt](../zephyr/CMakeLists.txt) et profils | Deux variantes, sélection des sources et entrées dédiées |
| [Kconfig OTA](../zephyr/modules/owntech_ota/zephyr/Kconfig), [CMake OTA](../zephyr/modules/owntech_ota/zephyr/CMakeLists.txt) | Exclusion réelle des fonctions Lead ; dépendances conditionnelles |
| `ota_runtime.cpp`, `ota_coordinator.cpp`, `ota_usb.cpp` | Séparation des rôles et suppression du staging bootable du firmware boards sur le Lead |
| `ota_participant.cpp`, `ota_storage.cpp`, `ota_protocol.*`, `ota_thingset.cpp` | Réception minimale, artefact compact, armement différé et reprise |
| `ota_safety.cpp`, `ota_feedback.cpp` et API Core | Adaptateur MMC, inhibition et suppression des réveils inutiles |
| [third_party/thingset-zephyr-sdk](../third_party/thingset-zephyr-sdk) | Capacités récepteur, limites et buffers versionnés ; aucune correction dans un cache |
| [ota_artifact.py](../owntech/scripts/ota_artifact.py), [ota_build_identity.py](../owntech/scripts/ota_build_identity.py) | Classes d'image, nouveau contrat signé, hashes et identités de build distincts |
| [pre_target_usb_lead.py](../owntech/scripts/pre_target_usb_lead.py), [lead_update.py](../owntech/tools/lead_update.py) | Construction de l'image récepteur, flux USB borné, exclusion du Lead des cibles |
| [provision_ota.py](../owntech/tools/provision_ota.py), outils de récupération | Vérification sans SMP applicatif, migration explicite et parcours USB seul |
| [tests/ota](../tests/ota), documentation opérateur | Régressions, pertes/coupures, mémoire et nouveaux workflows |

Le build ID doit inclure la variante, le profil et les sources effectives. Ne plus exiger l'égalité des identités des builds `OTA` et `USB_LEAD`. En revanche, toutes les boards d'une campagne doivent recevoir le même artefact récepteur vérifié. Archiver artefacts, manifestes et journaux hors des répertoires que PlatformIO nettoie.

## 10. Plan en commits incrémentaux

| Étape | Livrable | Vérification avant de poursuivre |
|---|---|---|
| 1. Référence reproductible | Mesure MMC USB/OTA, tailles et allocations connues, vérification d'intégrité du source MMC | Chiffres de référence reproduits sans modification de MMC |
| 2. Séparation des variantes | Runtime/API Lead séparés, binaire récepteur sans coordinateur ni tables de flotte | Deux builds ; contrôle des symboles et tailles ; tests de participant existants |
| 3. Initialisation minimale | Suppression SMP/CDC OTA des boards et ajout du statut USB borné | Initialisation d'une board seule, console/Scope utilisables, récupération sans ST-Link |
| 4. Contrat d'image sûr | Génération compacte, writer et journal versionnés, armement différé | Tests de bornes, hashes et coupures avant/après armement ; bootloader qualifié |
| 5. Distribution par Lead dédié | Source fichier PC, contrôle de débit, retransmissions, cibles sans le Lead | Lead inchangé après reset ; campagne interrompue puis reprise explicitement |
| 6. Réduction des buffers | Binaire ThingSet, capacités et pools réduits, file compacte, piles mesurées, LED événementielle | Mesure de chaque gain ; pertes et erreurs traitées sans débordement |
| 7. Intégration MMC dans Core/OTA | Adaptateur de maintenance/santé, mesure du tas et du temps réel | Source MMC identique ; arrêt sûr et reprise autorisée ; budget réel tenu |
| 8. Qualification et documentation | Matrice complète, journaux, tailles finales et procédure USB/CAN | Deux cycles complets consécutifs, coupures et flotte représentative |

Les tests unitaires de comportement précèdent le matériel. Les dépendances entre étapes priment sur un découpage artificiel : ne pas supprimer une voie de statut avant que sa remplaçante existe, ni lancer une campagne réelle avec le nouveau Lead avant validation du contrat d'image. Chaque commit décrit le changement et ses preuves ; aucune optimisation mesurée ne vaut à elle seule qualification de sûreté.

## 11. Critères d'acceptation

### Mémoire

- Image récepteur signée dans la borne utile qualifiée ; cible provisoire de surcoût au lien ≤83 000 octets.
- Cible de surcoût RAM de 25 000 octets, plafond de travail de 28 000 octets, abaissé si les autres allocations l'exigent. Pas de coordinateur ni de tableaux de flotte dans la carte mémoire réceptrice.
- Mesure du tas réel après initialisation MMC, activation de Scope et régime représentatif ; objectif de réserve réelle d'au moins 8 192 octets, frais d'allocateur compris. Justifier séparément les marges des piles avec leurs points hauts.
- Vérifier les ressources au repos **et pendant la maintenance**, car mettre MMC en pause ne libère pas ses allocations. Une allocation temporaire de campagne doit rentrer dans le même budget.
- Rapport final avec delta par rapport à MMC USB, fichier `.map`, configuration effective, taille de l'artefact signé et résultats des mesures dynamiques.

### Fonctionnement, erreurs et reprise

| Essai | Résultat attendu |
|---|---|
| Board seule USB, sans CAN ni ST-Link | Installation et confirmation locale ; attente CAN distincte d'un échec de santé |
| Lead seul USB | Installation de sa propre image et conservation de son rôle après reset |
| Deux campagnes consécutives | Même nouvelle image sur toutes les boards attendues, aucun réamorçage manuel normal |
| Image récepteur proposée comme mise à jour du Lead, et inversement | Refus explicite par les outils/services avant écriture |
| Nœud absent, EUI inattendu, doublon ou adresse changée | Pas de commit d'une liste partielle cachée ; identité stable vérifiée |
| Blocs perdus, dupliqués, désordonnés, tronqués ; CRC faux ; file pleine | Pas d'avancée incorrecte de l'offset ; réparation bornée ou échec explicite |
| Hash, classe, taille, version de protocole ou profil incompatible | Refus avant armement ; aucune écriture hors zone |
| Signature invalide malgré des hashes de transfert cohérents | Rejet effectif par la chaîne de signature/bootloader, aucune confirmation ni faux succès, ancienne image récupérable |
| Image active non confirmée, swap en attente, slot occupé ou NVS insuffisant | Refus avant effacement ; conservation de la copie de rollback et des données Core |
| Coupure avant PREPARE, pendant effacement/écriture, après VALID | Aucune activation de la candidate avant commit ; récupération en état sûr |
| Coupure pendant commit, après armement ou pendant swap | État durable interprétable, retour/essai géré, résultat partiel explicite, sorties inhibées |
| Santé postboot invalide ou mauvaise image active | Aucune fausse confirmation ni libération de maintenance |
| PC/Lead débranché, journal corrompu, ancien journal présent | Délais bornés, diagnostic et récupération ciblée ; conservation des données Core |
| Console USB occupée ou héritée, bootloader avec image non confirmée | Aucun reset/upload vers une cible ambiguë ; arrêt sans boucle de progression à 0 % |
| Requête RS485 de reprise pendant maintenance | Sorties maintenues inactives ; supervision indispensable maintenue |
| CAN silencieux puis chargé hors campagne | Latence HRTIM et pertes RS485 comparées à la référence MMC |

Qualifier d'abord un Lead et une board, puis la flotte réellement prévue. Les cycles historiques avec un Lead et un follower valident le prototype précédent sur ce banc ; ils ne prouvent ni cette nouvelle architecture, ni son comportement avec dix boards, ni sa résistance aux coupures.

## 12. Résultat attendu

Livrer un service récepteur dont la mémoire ne croît pas avec la flotte, une application Lead dédiée et des outils qui construisent puis distribuent l'image réceptrice sans la charger comme future application du Lead. Le workflow normal et sa récupération qualifiée doivent fonctionner par USB et CAN, sans ST-Link.

La compatibilité avec MMC_ANA sera déclarée seulement après mesure de l'image signée, de la RAM réellement disponible, des callbacks de sécurité et du comportement HRTIM/RS485. Le présent budget fournit une cible de travail ; la mesure finale décide de l'acceptation.
