# Plan système — mise à jour collective ThingSet sur CAN

## Statut et documents de référence

Ce document fixe l’architecture retenue : un ordinateur dépose une image dans
une carte Lead, qui la distribue sur un bus CAN commun avec ThingSet.
Il remplace l’ancienne proposition de transport OTA directement en raw CAN.
L’OTA collective décrite ici reste **à implémenter et à qualifier sur matériel**.

Le [rapport détaillé d’implémentation](thingset_can_ota_implementation_report.md)
décrit l’audit du dépôt et les étapes 0 à 11, avec les fichiers à modifier.
Le [plan d’adaptation du SDK ThingSet](thingset_can_broadcast_ota_action_plan.md)
précise les corrections de transport et les extensions nécessaires.
Les noms des nouvelles commandes, options et APIs sont des propositions,
pas des fonctions déjà disponibles dans Core.

## Objectif et périmètre initial

- Transférer une image depuis l’ordinateur vers la Lead une seule fois.
- Diffuser chaque passage de données une fois pour tous les participants.
- Utiliser ThingSet sur CAN pour les commandes, statuts et données.
- Conserver MCUboot pour l’installation locale et le mécanisme de test/rollback.
- Inclure la Lead parmi les cibles si le même firmware lui est compatible.
- Maintenir la puissance dans un état sûr pendant toute la maintenance.
- Rendre visibles les cartes absentes, incomplètes, en erreur ou déjà mises à jour.

La première version vise un seul bus, une seule Lead, la route ThingSet 0,
un groupe explicite de cartes compatibles et une image commune.
Elle utilise Classical CAN à 500 kbit/s comme point de départ à qualifier.
CAN FD est une optimisation ultérieure, conditionnée par tous les matériels,
le câblage, les paramètres des contrôleurs et les essais de transport.

La reprise d’un transfert après redémarrage, les images différentes par carte,
le routage entre bus et une activation atomique du groupe sont hors périmètre.

## Architecture physique et mémoire

```text
Ordinateur
  | USB CDC / serveur mcumgr dans l’application
  v
Lead : application actuelle dans image-0 ; image candidate dans image-1
  | ThingSet sur un bus CAN commun, câblé via RJ45
  +-----------------------+-----------------------+
  v                       v                       v
Carte A                 Carte B                 Carte C
image-0 : actuelle      image-0 : actuelle      image-0 : actuelle
image-1 : candidate     image-1 : candidate     image-1 : candidate
```

RJ45 désigne ici le connecteur portant les signaux CAN ; il ne signifie pas
Ethernet. Si les prises sont reliées au même bus, toutes les cartes reçoivent
les émissions de la Lead, même avec un câblage physique en chaîne.
Le brochage, les masses, les terminaisons et les longueurs restent à confirmer :
le dépôt ne suffit pas à prouver la topologie électrique de l’installation.

Le choix `thingset,can` doit correspondre au contrôleur activé par chaque shield.
Le `spin.dts` sélectionne actuellement `fdcan2`, alors que certaines révisions
de shields activent `fdcan1`. Vérifier le Devicetree final de chaque cible.

La Lead conserve son ancien programme pendant le dépôt et la distribution.
Elle lit son image secondaire ; elle ne réécrit pas sa propre diffusion CAN.
Les autres cartes écrivent leur mémoire localement : « simultanément » signifie
réception d’un flux commun, sans synchronisme exact des écritures flash.

## Préparation du firmware et des rôles

Toutes les cartes doivent déjà posséder un MCUboot approprié et une application
capable de recevoir l’OTA. Leur premier équipement passe par une procédure locale.
Un redémarrage dans MCUboot ne remplace pas le récepteur CAN applicatif.

Produire un nouvel artefact `firmware.ota.bin` signé, compact, avec les paramètres
d’en-tête et d’alignement attendus, **sans `--pad`, sans `--confirm` et sans
trailer préarmé**. Vérifier sa taille, sa signature et sa compatibilité.
Le fichier `firmware.mcuboot.bin` du chemin actuel ne doit pas être réutilisé :
la configuration `secondary_slot=1` conduit au padding destiné au chemin
d’installation existant et fournit un trailer préarmé.

Le hash de transport porte sur les octets exacts de `firmware.ota.bin`.
Les métadonnées de campagne précisent cette définition, la taille et la version.
La capacité utile doit tenir compte du mode MCUboot et de son trailer ; la taille
brute d’un slot ne constitue pas à elle seule la taille maximale de l’artefact.

Préférer un firmware commun avec rôle Lead/Follower à l’exécution, si la mémoire
le permet. La présence de l’USB ne doit pas autoriser deux coordinateurs concurrents.
Une campagne fixe l’identité de la Lead et la liste des identités participantes.
La compatibilité doit couvrir board, révision, shield, configuration et firmware.
Une flotte hétérogène peut nécessiter plusieurs campagnes et plusieurs images.

L’identité stable est distincte de l’adresse CAN courante. Publier une version
réelle du logiciel : le champ Core actuellement fixé à `"1.0.0"` ne permet pas
de contrôler une mise à jour. Un changement d’adresse exige une réassociation
prouvée à l’identité attendue, sinon l’arrêt de la campagne.

## Composants à ajouter ou adapter

| Composant | Responsabilité |
|---|---|
| Outil ordinateur | Construire/signaler le bon artefact, déposer, lancer et suivre |
| Serveur mcumgr applicatif Lead | Déposer sans activation ni redémarrage automatique |
| Service de stockage image | Contrôler l’accès exclusif au slot, écrire, vérifier, armer |
| Coordinateur Lead | Inventaire, barrières, passages, réparation et compte rendu |
| Participant OTA | Contrôles ThingSet, file de blocs, état et progression |
| Adaptateur ThingSet | Client adressé fiable et reports multi-trames corrigés |
| Intégration maintenance | Inhibition puissance, vérification et reprise contrôlée |

Core possède déjà des APIs CAN, flash et puissance, mais pas cette campagne.
L’activation de `CONFIG_THINGSET_DFU` seule ne réalise pas le multicast.
Les défauts identifiés du client SDK et du réassemblage multi-trames doivent
être corrigés et testés avant leur intégration dans une chaîne de mise à jour.
Conserver les révisions des dépendances et leurs modifications dans des sources
versionnées ; ne pas dépendre de correctifs manuels dans le cache PlatformIO.

## Règles de transport

Les contrôles fiables utilisent les requêtes/réponses **adressées** ThingSet :
`xPrepare`, `xBeginPass`, `xEndPass`, `xFinalize`, `xArm`, `xAbort` et les statuts.
La Lead sérialise les échanges si le client ne supporte qu’une requête en vol.
Les callbacks acceptent ou rejettent une commande ; les opérations longues
s’exécutent hors callback. Une réponse d’acceptation ne signifie pas `READY`
ou `VALID` : la Lead interroge ensuite l’état jusqu’au résultat ou au délai limite.

Les données `DATA` passent en broadcast dans le framing multi-trames ThingSet.
Leur enveloppe privée est une **extension de transport**, pas un report applicatif
ThingSet standard. Elle comporte un marqueur, une version, un type, l’identité
de campagne, le numéro de passage, l’offset, la longueur et un CRC.
Définir octet par octet l’encodage et les limites ; ne jamais transmettre
directement une structure C ni réaffecter arbitrairement des bits de l’ID CAN.

Après réassemblage, le callback vérifie rapidement taille/préfixe, copie dans un
pool préalloué et met en file sans attendre. Le worker vérifie source, campagne,
passage, état, bornes et intégrité ; lui seul possède le contexte d’écriture flash.
Le callback ne conserve pas un pointeur vers un buffer réutilisable du SDK.

L’offset publié `rOffset` avance après traitement séquentiel réussi par ce worker,
jamais au simple placement dans la file. Des octets peuvent encore être dans
le buffer du writer : cet offset ne promet pas leur persistance après reset.
La première version abandonne la session après reset et repart avec préparation.
Séparer les compteurs reçus/mis en file de cette progression d’écriture.

Les commandes et transitions doivent être idempotentes avec campagne et passage.
Une répétition compatible renvoie le résultat connu ; des paramètres différents
pour le même identifiant sont rejetés. Prévoir délais, nombre maximal de reprises,
erreurs d’overflow et de bus-off, et limitation du trafic pour la carte la plus lente.

## Déroulement d’une campagne

1. **Inventorier et verrouiller.** Identifier les cartes requises et la Lead,
   vérifier les versions/compatibilités et exclure toute autre écriture concurrente.
   Aucune carte absente ne doit être oubliée silencieusement.
2. **Mettre la Lead en maintenance avant l’USB.** Poser l’inhibition de puissance,
   vérifier l’état sûr, puis autoriser le service applicatif à écrire `image-1`.
   Le chemin USB habituel qui bascule dans le bootloader reste distinct.
3. **Déposer et vérifier la source.** Transférer `firmware.ota.bin`, terminer
   l’écriture, relire et vérifier taille/hash/compatibilité/signature selon le
   validateur choisi. Garder la source immuable jusqu’à la fin de la campagne.
4. **Préparer chaque participant.** Envoyer `xPrepare` avec les métadonnées.
   Chaque carte vérifie sa compatibilité, sa maintenance et son accès au slot,
   prépare son stockage, puis expose `READY`. Attendre toutes les cartes requises.
5. **Ouvrir le passage.** Envoyer `xBeginPass` adressé et attendre les états
   attendus. Le numéro de passage interdit la réutilisation de blocs anciens.
6. **Diffuser.** Lire la source séquentiellement et envoyer les blocs `DATA`
   une fois chacun. Pacer le flux pour le réassemblage, la file et la flash.
7. **Fermer et drainer.** Arrêter l’émission, envoyer `xEndPass` à chacun,
   puis attendre la fin du traitement des blocs acceptés avant de lire `rOffset`.
   La fermeture ne doit pas éliminer des blocs déjà en file.
8. **Réparer.** Une carte ignore les doublons sous son offset et n’écrit pas
   au-delà d’un trou. Reprendre un nouveau passage au minimum des offsets
   incomplets. Un nombre fini d’essais et de délais mène soit à la complétude,
   soit à une erreur explicite, sans armement.
9. **Finaliser.** Envoyer `xFinalize` adressé. Vider le buffer flash, relire
   les octets stockés, vérifier taille/hash et validité de l’image MCUboot.
   La Lead effectue la même validation locale. Attendre tous les états `VALID`.
10. **Armer.** Envoyer `xArm` seulement après cette barrière de validation.
    Chaque carte signale `ARMED` uniquement après réussite de l’armement MCUboot.
    La Lead est également armée ; un échec à ce stade produit un état partiel.
11. **Redémarrer.** Après observation de tous les `ARMED`, diffuser `REBOOT`
    avec campagne et délai borné, puis redémarrer la Lead après transmission.
    Les répétitions ne doivent pas repousser indéfiniment un reboot déjà programmé.
12. **Contrôler le résultat.** Chaque nouveau firmware démarre avec puissance
    inhibée, effectue ses vérifications locales et applique la politique de
    confirmation MCUboot. Ensuite, réinventorier toutes les cartes et comparer
    identité/version/état. La reprise électrique exige une validation explicite.

## Sûreté et propriété des ressources

`shield.power.stop(ALL)` est une primitive utile, mais ne fournit ni verrou
maintenance ni preuve suffisante de l’état électrique de chaque installation.
Les tâches applicatives, les entrées CAN et les APIs PWM peuvent relancer les
sorties : leur inhibition fait partie du contrat, dès avant la première écriture.

`task.stopCritical()` arrête aussi l’appel périodique de `safety_task()` dans
l’intégration actuelle. Maintenir une supervision adaptée au mode maintenance.
`disableSafetyApi()` ne constitue jamais une commande d’arrêt de puissance.
Les vérifications requises et les conditions de reprise sont propres à l’application.
Avant la première écriture d’image, persister et vérifier l’inhibition maintenance.
La restaurer au boot avant toute autorisation de puissance et avant `setup_routine`,
dans le firmware actuel, le nouveau et celui exécuté après rollback. Un journal
illisible impose la maintenance par défaut ; un simple verrou RAM ne suffit pas.

Avant `stage_begin` ou `xPrepare`, exiger une image active confirmée et un état boot
compatible avec un nouveau transfert. Refuser les états test/non confirmé,
pending, revert ou inconnus : le slot secondaire peut contenir la copie de rollback
précédente. Un arbitre RAM seul ne protège pas cette image après un reset.
Un seul service possède le slot secondaire : dépôt USB, réception OTA, DFU
unicast, effacement et armement ne doivent pas s’exécuter concurremment.
La file OTA est distincte du buffer global actuellement utilisé pour les items CAN.
Réserver les IDs et empêcher le chemin de commande normal d’importer des objets OTA.

Le stockage NVS Core contient déjà calibration, seuils et métadonnées dans 4 Kio.
Si rôle ou journal sont persistés, partager son propriétaire et réserver des clés.
Ne pas monter un second backend sur la même partition ni effacer les données
utilisateur pour remettre une campagne à zéro. Mesurer aussi l’espace restant.

## Limites de validation, panne et récupération

La barrière avant `ARM` garantit que l’armement n’est demandé qu’après validation
de tous les participants sélectionnés. Elle ne garantit pas un commit atomique.
Après un premier armement, une panne de la Lead, un reset ou une perte CAN peut
laisser certaines cartes armées ou déjà démarrées et d’autres dans l’ancien état.

Avant tout armement, `xAbort` abandonne la session sans activer l’image.
Après armement, `xAbort` ne doit pas prétendre effacer une intention MCUboot :
rapporter les états individuels et appliquer une récupération documentée.
Ne pas effacer aveuglément un slot ou son trailer pour « annuler » une campagne.

Le rollback MCUboot est local et dépend de la configuration du bootloader,
du mode test, de la confirmation et d’un redémarrage effectif.
La santé locale précède l’inventaire global ; ni l’absence d’une carte ni une
version différente ne déclenchent implicitement un rollback collectif garanti.

Après reset pendant réception, abandonner la session de transfert en RAM.
L’absence de reprise du transfert ne dispense pas de réconcilier le journal,
l’inhibition maintenance et l’état MCUboot avant une nouvelle préparation complète.
Après panne après armement, inventorier versions et états avant toute nouvelle
écriture ; maintenir la puissance inhibée tant que le groupe est incohérent.
Prévoir les procédures de récupération USB/STLink pour chaque carte et la Lead.

## Validation et critères d’acceptation

| Essai | Résultat requis |
|---|---|
| Image compacte déposée, puis reset non demandé | Aucun trailer actif ne provoque une activation prématurée |
| Lead + au moins deux cartes compatibles | Un flux commun, images stockées identiques, versions finales vérifiées |
| Mauvais board/shield ou image trop grande | Rejet avant données et avant armement |
| Fragment perdu, report perdu, doublon, mauvais passage | Offset cohérent, réparation bornée, aucune écriture hors ordre |
| Fin de passage avec blocs encore en file | Drainage avant décision de réparation/finalisation |
| File pleine, flash lente ou échec flash | Erreur/progression cohérentes et aucune fausse validation |
| Source inconnue, ancienne campagne, double Lead | Rejet sans altérer la session active |
| Bus-off ou timeout d’une carte requise | Arrêt/échec explicite ; pas d’armement prématuré |
| Reset avant ARM, puis après ARM | Ancienne image préservée avant ARM ; état partiel rapporté après ARM |
| REBOOT répété/perdu | Idempotence et absence de déclaration de succès sans inventaire |
| Commande start pendant maintenance et au reboot | Sorties restent inhibées ; vérification matérielle |
| USB/unicast/NVS en concurrence | Propriété respectée, calibration et seuils préservés |
| Journal illisible, reset ou rollback | Maintenance restaurée avant toute reprise de puissance |
| Nouvelle campagne avec image active en test | Refus du dépôt/PREPARE et copie de rollback intacte |
| Classical CAN puis FD qualifié | Intégrité identique, cadence et consommation mémoire mesurées |

La recette finale doit enregistrer les identités participantes, révisions exactes
des sources et bootloaders, paramètres CAN, artefact/hash, tailles Flash/RAM,
durées, reprises, erreurs et résultats après redémarrage.
La réussite d’un échange CAN ou du seul hash ne suffit pas à déclarer la flotte
mise à jour : toutes les cartes requises doivent être retrouvées dans l’état prévu.