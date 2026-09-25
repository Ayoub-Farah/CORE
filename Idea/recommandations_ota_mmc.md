# Incident OTA du MMC : problèmes identifiés et prévention

État au 25 septembre 2026 : réparation individuelle des deux cartes (SM2 puis SM1), découplage CAN/OTA, puis première campagne CAN complète réussie avec le nouveau main sans callbacks OTA.

## Ce qui a posé problème

**Deux défauts de l'application empêchaient la confirmation du MMC : les cartes n'étaient pas reconnues par sa configuration et le contrôle de santé d'un follower dépendait du signal SYNC de SM1.** Le premier défaut était présent dans l'image transférée ; le second a été constaté sur SM2 après correction de son identité.

Le transfert réussi signifiait que l'image avait été reçue et validée. Il ne prouvait pas que le MMC pouvait démarrer correctement sur ces cartes. Le blink précédent n'avait pas les mêmes contraintes d'identification et de synchronisation.

| Problème | Conséquence | Niveau de certitude |
|---|---|---|
| UID des deux cartes absents de la configuration MMC | `module_ID=0`, initialisation interrompue, santé refusée | Confirmé dans l'image transférée |
| Santé de SM2 conditionnée à un cycle de contrôle déclenché par SYNC | Impossible de confirmer SM2 tant que SM1 n'envoyait rien | Confirmé sur SM2 après correction de son UID |
| Journal attendant encore la nouvelle image après retour arrière | Refus d'une nouvelle campagne : `RECOVERY_REQUIRED`, erreur `-18` | Confirmé sur les deux cartes |
| Erreur d'identité du Lead pendant commit/redémarrage | Campagne non achevée, erreur `-4` | Erreur confirmée, cause exacte encore à déterminer |

### 1. La configuration ne correspondait pas aux cartes

La table compilée contenait notamment `0x0033004C` pour SM1 et `0x0031001B` pour SM2. Les cartes du banc avaient d'autres UID :

| Carte | Série USB | UID utilisé par le MMC | Rôle désormais configuré |
|---|---|---|---|
| Première | `3232500B0029004C` | `0x0029004C` | SM1 |
| Seconde | `3232500B00290049` | `0x00290049` | SM2 |

Pour une carte inconnue, `detect_module_id()` renvoyait `0`. `setup_routine()` faisait alors un `return` avant de terminer l'initialisation. Le contrôle de santé échouait et ne confirmait pas l'image.

**Le `return` n'a pas directement déclenché le retour arrière et n'a pas détruit les cartes.** Il a empêché l'initialisation, donc la confirmation. Lors d'un redémarrage ultérieur, MCUboot peut revenir à l'ancien firmware si l'image d'essai n'a pas été confirmée : c'est son mécanisme de protection. Voir la [documentation MCUboot](https://docs.mcuboot.com/design.html#image-swapping).

Sur les deux cartes, les sauvegardes SWD et les marqueurs flash prouvent un `REVERT` terminé. La source exacte du redémarrage reste inconnue.

### 2. La santé locale de SM2 dépendait à tort de SM1

Après correction de l'UID, SM2 reconnaissait son rôle et terminait son initialisation. Son compteur de cycles de contrôle restait cependant à zéro : la tâche critique d'un follower est exécutée sur les interruptions SYNC externes, et SM1 était bloquée.

Le contrôle de santé exigeait un nouveau cycle avant de confirmer le firmware. Une dépendance collective — la présence du maître — empêchait donc de valider le démarrage local d'une carte initialisée et maintenue à l'arrêt.

**Corriger seulement les UID ne suffisait donc pas.** Il fallait distinguer la santé locale de SM2 de la disponibilité du MMC complet.

### 3. Pourquoi les flashs suivants étaient refusés

La campagne avait dépassé le commit. Après le retour à l'ancien firmware, son journal persistait et attendait toujours l'image MMC. Le firmware actif ne correspondait plus à l'image attendue : le récepteur refusait une autre campagne avec `RECOVERY_REQUIRED` et `OTA_ERR_HEALTH` (`-18`).

Ce refus protégeait un état non résolu. Il ne signalait pas une flash irrécupérable. Relancer le transfert ne pouvait pas le corriger : il fallait vérifier les images et traiter la campagne interrompue.

### 4. Ce que l'on sait de l'erreur d'adresse

Les adresses CAN peuvent changer après redémarrage : ThingSet choisit une autre adresse si celle demandée est occupée. L'identité matérielle reste stable.

Le Lead prévoit déjà une redécouverte après redémarrage et possède des contrôles de changement ou de collision d'adresse. **La cause exacte du `OTA_ERR_IDENTITY` (`-4`) observé n'est pas encore établie.** Ne pas l'attribuer avec certitude au simple redémarrage, ni supprimer les contrôles d'identité pour le contourner.

## Ce qui est corrigé, et ce qui ne l'est pas encore

| Point | État |
|---|---|
| UID réels associés à SM1 et SM2 | Corrigé dans le code, testé |
| Confirmation locale d'un follower sans SYNC | Corrigée, testée et vérifiée sur SM2 |
| Contrôle de santé installé lors des réparations | Identité MMC, initialisation, tâche de fond, sorties arrêtées ; cycles locaux exigés pour SM1 seulement |
| Découplage du code actuel | Confirmation du socle CAN/OTA sans callback applicatif ; déploiement et disponibilité vérifiés sur les deux cartes avec le nouveau main |
| Campagne bloquée sur SM2 | Réparée par SWD après sauvegardes et vérification du retour arrière |
| Campagne bloquée sur SM1 | Réparée à partir de ses propres sauvegardes ; même image corrigée installée et confirmée |
| Vérification des rôles avant transfert | Pas encore implémentée |
| Cause du `-4` du Lead | À investiguer |
| Cycle OTA complet avec le nouveau MMC sur deux récepteurs | Réussi : campagne `6169884459129618946`, deux cartes confirmées et disponibles |

**Résultat après les réparations individuelles, avant le déploiement du découplage : `IDLE`, erreur `0`, firmware confirmé automatiquement, santé locale et CAN valides, récepteur disponible en OTA.** Les deux cartes exécutaient alors le même build `ota-d8f4b20f4ffa5d7e56c19ecf`. Les sorties ont été vérifiées arrêtées ; les bootloaders, secondaires et calibrations enregistrées ont été préservés.

La confirmation locale ne valide pas le fonctionnement de puissance ni la perte de SYNC en exploitation. Le cycle complet via le Lead a ensuite réussi, comme décrit plus bas. Les détails des réparations, hashes et preuves sont dans les rapports [SM2](../recovery-backups/2026-09-25-3232500B00290049/README.md) et [SM1](../recovery-backups/2026-09-25-3232500B0029004C/README.md).

## Découplage réalisé : remplacer le main sans réécrire CAN/OTA

Les services étaient déjà dans des fichiers et des threads séparés, mais leur confirmation et leur entrée en maintenance appelaient encore le code de l'application. Déplacer simplement les fonctions hors de `main.cpp` n'aurait donc pas suffi.

Le socle prend maintenant en charge :

- Le démarrage automatique des threads CAN et OTA, sans appel depuis `main`.
- La confirmation après contrôle du contrôleur CAN, du stockage, de l'image active et des sorties inhibées. Aucun callback `owntech_ota_check_health()` ou `owntech_ota_enter_maintenance()` n'est requis ni appelé, même s'il reste dans un ancien main.
- L'arrêt direct des douze sorties HRTIM, avec activation préalable de leur horloge, ainsi que la configuration et la relecture des GPIO de puissance. L'arrêt ne dépend plus des masques PWM initialisés par `shield.power.initBuck()`.
- Des priorités permettant aux services de préempter le main : OTA `7`, CAN/SDK `10`, main `12`. Dans Zephyr, le plus petit nombre est prioritaire. Une configuration qui permettrait au main de bloquer ces services est refusée à la compilation. Voir [l'ordonnancement Zephyr](https://docs.zephyrproject.org/latest/kernel/services/scheduling/index.html).
- La protection de la réserve NVS : les écritures applicatives sont refusées avec `-EBUSY` pendant l'inhibition ou une opération OTA ; les enregistrements OTA restent autorisés. L'effacement global par l'API applicative renvoie `-EPERM` dans les builds OTA.

**Utilisation : remplacer `src/main.cpp` et compiler avec l'environnement `OTA`.** Aucun include ou callback OTA n'est obligatoire. Un main qui retourne, dort, attend SYNC ou boucle normalement ne doit pas empêcher les services de tourner. L'environnement `USB_LEAD` conserve son application dédiée ; l'environnement ordinaire `USB` ne garantit pas la présence du récepteur OTA.

Le MMC utilisé pour la validation logicielle du découplage conservait une observation facultative de l'inhibition pour gérer ses commandes et son réarmement après un nouvel état IDLE. Le main remplacé ensuite par l'utilisateur ne reprend pas cette observation. La disponibilité de CAN/OTA ne dépend pas de ces fonctions applicatives ; le refus de puissance pour une carte inconnue et la gestion des commandes après inhibition doivent être vérifiés pour chaque application.

**Le sens de « santé » change explicitement : il s'agit de la santé du service de mise à jour.** Une image confirmée peut contenir une application MMC défaillante ; son diagnostic et son autorisation de puissance restent distincts. Les contrôles de journal, de hash, de retour arrière et de sécurité matérielle ne sont pas contournés. Un journal incohérent reste un motif légitime de refus.

### Limite de cette indépendance

Ce découplage partage encore le processeur, la mémoire et les privilèges avec l'application. Il ne garantit pas une mise à jour après un HardFault, une désactivation permanente des interruptions, un verrouillage de l'ordonnanceur, une boucle prioritaire, un constructeur global bloquant ou une reconfiguration directe du CAN, de la flash ou des sorties. Les clés NVS `0x0500` à `0x0503` restent réservées au socle ; l'API n'est pas une isolation contre du code écrivant volontairement sur ces clés.

Pour récupérer même dans ces situations, l'étape suivante serait un **bootloader de récupération CAN indépendant**, joignable après reset, avec une politique de watchdog et d'entrée en récupération qualifiée sur le matériel. Ce mécanisme n'est pas ajouté par le présent découplage. Il faut aussi garder assez de temps CPU, de RAM et de marge sur les interruptions pour le contrôle et CAN/OTA.

### Vérification logicielle du découplage

Lors de la validation du découplage, avant le remplacement du main, la commande `python -m unittest discover -s tests/ota -p '*test*.py'` a terminé sans échec : 386 tests, dont un ignoré parce que la création de liens symboliques exige des privilèges Windows supplémentaires. Elle couvrait notamment les runtimes récepteur/Lead sans hooks applicatifs, l'arrêt matériel avant initialisation PWM, les gardes NVS, les priorités et le réarmement MMC. Les tests matériels utilisent des substituts ; ils ne mesurent ni le temps réel ni les signaux électriques.

Le build `OTA` du MMC sans callbacks produit une image CAN de 204 828 octets. Les images USB et CAN ont une signature vérifiée par `imgtool` et le même hash MCUboot `4d946d879d9df7ab4b5f97763e287f6a1c7a5fb61a0d091fefc41a0cfff6f789` (build `ota-d0130040168cf4c8cf56ab2d`). Les priorités 12/10/10 sont présentes dans la configuration compilée. Les journaux de compilation sont conservés dans [ota-artifacts/qualification/ota-autonomy](../ota-artifacts/qualification/ota-autonomy/).

Le build `USB_LEAD` réussit également, avec une image compacte de 198 052 octets ; ses signatures USB/CAN sont vérifiées et leur hash MCUboot correspond. Aucun de ces nouveaux builds n'a été flashé pendant cette validation logicielle. Les essais de disponibilité face à un main occupé, les deux campagnes CAN consécutives et les mesures de temps réel restent à réaliser sur le banc.

### Premier déploiement du découplage depuis le Lead

Après remplacement du main par l'utilisateur, l'image `ota-7c602295d5a07efc4667f737` a été compilée et transférée aux deux cartes lors de la campagne `6169884459129618946`, terminée avec succès le 25 septembre à 15:58:42 UTC. Ce main ne contient aucune fonction OTA. Seuls les UID SM1/SM2, revenus aux anciennes valeurs, ont été rétablis selon l'affectation convenue.

Les deux cartes exécutent le hash MCUboot `8b6a363c70c3fbd914faaafcaf5172062c03b5884a0c01f9ea7aa85587a2aa6f`, confirmé automatiquement. Leur état final est `SUCCESS`, erreur `0`, `healthy=true`, `confirmed=true`, `available=true`. Les adresses CAN ont changé de `7` à `52` pour SM1 et de `93` à `155` pour SM2 ; le Lead les a retrouvées par EUI et a terminé la réconciliation sans erreur `-4`.

Le Lead n'a pas été reflashé. Aucun accès ST-Link, reset forcé ni effacement de journal n'a été nécessaire. Le main déployé attend un maître MMC distinct ; la réussite OTA ne valide pas son fonctionnement de puissance. Une deuxième campagne consécutive et les essais d'un main volontairement occupé restent à faire. Voir le [rapport et les preuves de campagne](../ota-artifacts/operations/20260925T155432Z-main-can-update/README.md).

## Recommandations pour éviter une récidive

### Priorité 1 — Vérifier les rôles de la candidate avant le transfert

**Problème évité : installer une image qui ne reconnaît pas les cartes.**

Ajouter une vérification après découverte des récepteurs et avant leur préparation OTA, donc avant l'effacement du secondaire. Comparer leurs identités avec les rôles supportés par **l'image candidate exacte** ; refuser les cartes inconnues et les rôles dupliqués. Vérifier aussi le matériel, le protocole applicatif et la topologie attendue.

Le contrôle actuel de compatibilité OTA ne couvre pas cette affectation MMC. Exemple de refus souhaité : `Carte 3232500B00290049 absente de la configuration MMC candidate. Aucun transfert démarré.`

Générer la table compilée et les métadonnées PC depuis une configuration commune. Lier ces métadonnées au binaire, idéalement par la signature de l'image. Lire uniquement le `main.cpp` courant ne suffit pas : l'image sélectionnée peut provenir d'une autre compilation.

Documenter la correspondance UID matériel / EUI ThingSet / rôle MMC. L'inventaire OTA expose l'EUI, alors que le MMC utilise un UID court : prévoir une correspondance fiable ou exposer l'identité nécessaire. Si la vérification est impossible, le signaler explicitement. À terme, préférer une identité matérielle complète à un identifiant partiel.

### Priorité 2 — Séparer santé OTA, santé applicative et autorisation de puissance

**Problème évité : bloquer un follower sain parce que son maître est absent, ou autoriser la puissance trop tôt.**

| Validation | Moment | Condition attendue |
|---|---|---|
| Compatibilité de la candidate | Avant transfert | Identités, rôles, matériel et protocoles compatibles |
| Santé du socle OTA | Après démarrage, avant confirmation | Contrôleur CAN démarré, stockage et image cohérents, sorties arrêtées ; aucun pair CAN exigé pour confirmer localement |
| Santé applicative MMC | Avant autorisation de puissance | Rôle connu, initialisation et tâches de contrôle valides |
| Disponibilité collective | Avant autorisation de puissance | Synchronisation, communications, mesures et topologie conformes aux exigences du MMC |

Un follower doit pouvoir attendre SYNC tout en restant accessible en OTA. L'absence de cycles de SM1 est un défaut applicatif à diagnostiquer ; elle ne doit plus bloquer la confirmation du service OTA. Cette séparation est implémentée dans le code actuel.

Conserver les délais bornés et les contrôles du socle avant confirmation. Refuser la puissance sur une carte inconnue, sans rendre le récepteur OTA indisponible. Ne pas réintroduire de callbacks bloquants du main dans le démarrage ou la maintenance du socle.

Maintenir les conditions de réarmement et de communication avant la puissance. Tester séparément la disparition de SYNC en fonctionnement et l'arrêt effectif des sorties : ce scénario n'est pas qualifié par un démarrage sans SYNC.

### Priorité 3 — Suivre les identités malgré les changements d'adresse CAN

**Problème évité : prendre une nouvelle adresse pour une nouvelle carte ou une collision.**

Auditer le `-4` observé. Figer les identités de la campagne, puis actualiser leurs adresses après redémarrage. Vérifier les transitions commit/redémarrage/réconciliation et les associations périmées.

Conserver les refus des collisions réelles et identités inattendues. Avant de déclarer une carte à jour, vérifier identité, campagne, image active et confirmation. Journaliser l'ancienne adresse, la nouvelle, l'EUI et la phase exacte du refus. Ne pas imposer des adresses fixes uniquement pour masquer cet incident.

### Priorité 4 — Prévoir la récupération après commit ou retour arrière

**Problème évité : une nouvelle campagne impossible après le retour protecteur à l'ancien firmware.**

Prévoir une procédure dédiée qui vérifie l'identité, les deux images, les marqueurs de démarrage et le journal, puis résout uniquement l'état OTA concerné. Préserver bootloader, calibrations et données non OTA, et prévoir les interruptions pendant la réparation.

Le mode existant `--compact-receiver-only` concerne une récupération avant commit ; il ne couvre pas cet incident. Chaque carte a été réparée à partir de ses propres preuves, notamment de sa disposition NVS. Ces réparations ciblées ne constituent pas une procédure générique à rejouer sans nouvelle vérification.

Archiver journaux, artefacts et sauvegardes hors des dossiers de compilation jetables. Ne pas effacer globalement la flash, supprimer les fichiers d'opération ou forcer une confirmation pour faire disparaître un refus.

### Priorité 5 — Afficher et conserver la cause de l'échec

**Problème évité : devoir utiliser le ST-Link pour comprendre chaque erreur `-18`.**

Distinguer carte inconnue, initialisation incomplète, tâche locale bloquée, attente de SYNC, échec de confirmation et retour à une autre image. Conserver l'image candidate, le rôle détecté, l'étape atteinte et la cause de reset lorsqu'elle est disponible.

Des messages locaux ont été ajoutés au MMC. La remontée structurée et la conservation après retour arrière restent à réaliser. Dimensionner ces diagnostics pour la capacité NVS et limiter les écritures afin de préserver les transitions critiques du journal OTA.

Dans l'interface, distinguer réception, validation, activation, santé, confirmation et fin de campagne. Le message « réussi » doit préciser l'étape réellement accomplie.

## Tests à conserver avant les prochains déploiements

| Scénario | Résultat attendu | État |
|---|---|---|
| UID réels et UID inconnu | Rôles corrects ; puissance refusée à l'inconnu | Test logiciel ajouté |
| SM2 démarre sans SM1/SYNC | Confirmation locale possible, sorties arrêtées | Vérifié sur SM2 avec la version de réparation ; nouveau découplage à vérifier sur carte |
| Aucun callback applicatif, ou anciens callbacks refusants | Les contrôles du socle fonctionnent sans appeler l'application | Tests logiciels ajoutés |
| Arrêt avant toute initialisation PWM du main | Toutes les sorties HRTIM arrêtées et GPIO vérifiés | Tests logiciels ajoutés |
| Sorties actives ou erreur de relecture GPIO | Santé du socle refusée | Tests logiciels ajoutés |
| Main qui retourne ou boucle sans dormir | CAN/OTA toujours joignable si interruptions et ordonnanceur opérationnels | Priorités contrôlées à la compilation ; essai sur carte à faire |
| Écriture NVS applicative en maintenance, ou effacement global | Refus ; journal OTA préservé | Tests logiciels ajoutés |
| Carte inconnue, rôle dupliqué ou configuration différente du binaire | Refus avant effacement du secondaire | À ajouter avec la vérification préalable |
| Changement d'adresse CAN après redémarrage | Même identité retrouvée sans fausse collision | Vérifié sur les deux cartes lors de la campagne réussie ; ancienne cause du `-4` non démontrée |
| Retour arrière après commit | Diagnostic précis et récupération maîtrisée | Réparations SM1/SM2 réussies ; procédure générale à prévoir |
| Perte de SYNC en exploitation | Arrêt des sorties selon les exigences MMC | À qualifier |
| Coupure pendant activation ou récupération | Reprise contrôlée, données non OTA préservées | À qualifier |
| Deux mises à jour consécutives à deux récepteurs | Confirmation et disponibilité sans réparation manuelle | À refaire avec le MMC corrigé |

Le code prévoit dix modules et cinq modules par bras. Définir explicitement un mode de banc à deux cartes avec sorties inhibées et critères adaptés. Les essais locaux SM1/SM2 ne constituent pas une validation du MMC complet.

Vérifier aussi les coefficients de mesure avant un essai de puissance : la conservation des calibrations en NVS ne garantit pas que l'application les utilise sans modification. Le code SM1 applique des coefficients explicites en RAM ; ils doivent correspondre à la carte réellement affectée à ce rôle.

## Prochaines actions

1. Compléter le premier déploiement réussi par les essais d'un main minimal et d'un main occupé sur carte.
2. Ajouter la vérification applicative avant transfert et la configuration commune liée à l'image.
3. Déterminer la cause du `-4` du Lead et vérifier le suivi des adresses.
4. Valider deux campagnes consécutives avec les deux récepteurs, puis qualifier séparément la puissance et la perte de synchronisation.

## Références du diagnostic

- [Code MMC](../src/main.cpp) et [tests applicatifs](../tests/ota/mmc_application_test.py).
- [Client PC de mise à jour](../owntech/tools/lead_update.py).
- [Lead : identités et réconciliation](../zephyr/modules/owntech_ota/zephyr/src/ota_lead_runtime.cpp).
- [Récepteur : contrôles au démarrage](../zephyr/modules/owntech_ota/zephyr/src/ota_receiver_runtime.cpp).
- [SYNC du MMC](../zephyr/modules/owntech_communication/zephyr/src/SyncCommunication.cpp) et [déclenchement de la tâche critique](../zephyr/modules/owntech_hrtim_driver/zephyr/src/hrtim.c).
- [Campagne échouée 4272146613398954838](../ota-artifacts/operations/20260925T143259Z-eaf5b8c377d2/campaign.jsonl).
- [Inspections USB des deux cartes](../ota-artifacts/operations/20260925T143259Z-eaf5b8c377d2/diagnostics/).
- [Procédures et limites de récupération](../docs/minimal-can-ota.md).

Les preuves matérielles sont locales ; les conserver avec les artefacts exacts de l'intervention, même si elles ne sont pas suivies par Git.
