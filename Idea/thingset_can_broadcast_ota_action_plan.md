# Plan d'adaptation du SDK ThingSet pour l'OTA collective sur CAN

Statut : proposition d'implémentation issue de l'audit du 23 septembre 2026. Aucun changement logiciel décrit ci-dessous n'est réalisé par cette documentation.

Ce document précise les modifications du transport et du service OTA. Le [rapport d'implémentation principal](thingset_can_ota_implementation_report.md) décrit le dépôt, le démarrage, le stockage et les étapes transversales. Le [plan système](can_broadcast_ota_action_plan.md) définit le fonctionnement ordinateur → Lead → cartes.

## 1. Décision d'architecture

Toutes les cartes d'une campagne reçoivent le même artefact compatible avec leur matériel. L'ordinateur dépose cet artefact sur la Lead par USB/mcumgr, pendant que l'application de la Lead continue de fonctionner. La Lead lit son slot secondaire et émet les données une seule fois sur le bus CAN commun.

Les récepteurs écrivent chacun leur propre slot secondaire. « Simultané » signifie que plusieurs cartes progressent à partir du même flux diffusé ; cela ne signifie ni écritures synchronisées à la microseconde, ni activation atomique du groupe.

Périmètre V1 :

- Un seul bus physique CAN, une instance ThingSet CAN par carte, route 0.
- Une seule Lead et une seule campagne active.
- Une image commune, compacte et signée, appelée ici firmware.ota.bin.
- Artefact produit sans --pad, sans --confirm et sans trailer d'activation prérempli.
- Contrôle et statut par requêtes/réponses ThingSet adressées.
- Données par diffusion multitrames utilisant l'enveloppe CAN ThingSet de type 0x1.
- Redémarrage collectif demandé par un message diffusé, après vérification des états ARMED.
- Aucune reprise de transfert après reset ; réconcilier journal/MCUboot, puis préparer une nouvelle réception si cet état l'autorise.
- Aucun engagement de bascule ou de rollback atomique de toutes les cartes.

Le transport proposé pour les blocs bruts est une **extension privée OwnTech**, pas un nouveau format normalisé ThingSet. Les messages ThingSet ordinaires restent identifiables et fonctionnels.

## 2. Révisions et périmètre de l'audit

Le manifeste [west.yml](../west.yml) épingle :

| Composant | Révision vérifiée |
|---|---|
| ThingSet Zephyr SDK | e57447bbeb7c165e14a273242e8495343c1c6f54 |
| thingset-node-c | 68c7544830df2ba23f67e31bad91e124377827a3 |

Les sources présentes sous le cache PlatformIO framework-zephyr/_pio/ ont été lues ; leurs HEAD correspondent à ces révisions et leurs arbres étaient propres lors de l'audit. Ces caches constituent une preuve de lecture, pas l'emplacement où développer les changements.

Maintenir un fork SDK dans un dépôt de développement normal, tester les correctifs, puis modifier le manifeste vers le commit retenu. Ne jamais livrer une modification manuelle du cache téléchargé par PlatformIO.

Les conclusions ci-dessous reposent sur une analyse statique. Aucun essai CAN, flash, boot ou injection de panne n'a été exécuté pour ce rapport. La configuration de build USB présente dans le workspace désactive CAN ; elle ne valide donc pas cette architecture.

## 3. Ce qui existe et ce qui manque

| Fonction | État constaté | Travail requis |
|---|---|---|
| Identité EUI et revendication d'adresse | Présentes | Inventaire actif, contrôle DLC, gestion des changements |
| Serveur ThingSet CAN adressé | Présent | Préserver et tester pendant OTA |
| API cliente CAN de la Lead | Déclarée, défaut critique de fin TX | Corriger avant toute coordination |
| Reports multitrames | Émission et réassemblage présents | Corriger erreurs, reprise FIRST, timeout et concurrence |
| Réception de payload brut | Callback existant | Dispatcher unique et copie dans une file bornée |
| Émission de payload brut | Absente | Factoriser l'émetteur et ajouter une API |
| DFU séquentiel unicast | Présent | Service legacy optionnel, ownership du slot |
| Campagne collective | Absente | États, commandes, réparation, barrières |
| Version de firmware exploitable | Champ local constant | Publier l'identité réelle de l'artefact |

Les IDs CAN sont définis dans [can.h, description des enveloppes](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/include/thingset/can.h#L19-L81). Le DFU actuel se trouve dans [dfu.c](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/src/dfu.c#L20-L117).

## 4. Correctifs préalables du client requête/réponse

### 4.1 Séparer fin d'émission et réponse applicative

L'API publique thingset_can_send() est déclarée dans [can.h](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/include/thingset/can.h#L383-L392). Elle installe une transaction attendant une réponse, puis transmet par ISO-TP.

Cependant, [le callback de fin TX](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/src/can.c#L555-L572) appelle le callback applicatif et efface la transaction même si l'émission réussit. ISO-TP invoque effectivement cette fin TX dès l'envoi terminé, pour une [trame simple](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/subsys/canbus/isotp_fast/isotp_fast.c#L1085-L1124) comme pour un [message multitrames](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/subsys/canbus/isotp_fast/isotp_fast.c#L839-L858).

La réponse réelle peut alors être traitée comme une nouvelle requête. Le coordinateur ne doit pas réutiliser cette API sans correctif.

Comportement attendu :

1. Réserver une transaction et identifier le destinataire, la route et sa génération.
2. Installer l'attente avant d'émettre, car la fin TX peut être synchrone.
3. Sur succès TX, conserver l'attente de réponse.
4. Sur réponse attendue, fournir les octets reçus et terminer la transaction.
5. Sur erreur TX, erreur RX corrélée ou timeout, terminer avec une erreur précise.
6. Garantir un seul callback terminal, même si timeout et réponse arrivent ensemble.
7. Diagnostiquer une réponse tardive sans la considérer comme une nouvelle commande OTA.
8. Autoriser une seule requête cliente active par instance en V1.

Les réponses OTA doivent rappeler la campagne et, selon la commande, la passe ou le commit. Cette corrélation applicative complète la correspondance source/route ; elle distingue une réponse tardive d'une réponse à un retry.

### 4.2 Durée et propriété des buffers

Pour les messages longs, ISO-TP conserve le pointeur fourni jusqu'à la fin d'émission ; il ne copie pas immédiatement tout le message. Un buffer local de pile réutilisé après thingset_can_send() n'est donc pas un contrat sûr.

Créer un contexte d'émission distinguant requête cliente, réponse serveur et possession éventuelle du buffer partagé. Libérer uniquement le buffer effectivement détenu par cette émission.

Le code actuel peut libérer le sémaphore global sans connaître son propriétaire. Ce point doit être corrigé avant de faire cohabiter le client Lead, les réponses serveur et les reports périodiques.

Le callback recevant une réponse doit copier les données utiles avant leur réutilisation. Il ne doit pas lancer une nouvelle requête bloquante depuis le contexte qui traite la réponse.

### 4.3 Erreurs et bornes

Dans [can.c, réception et erreurs](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/src/can.c#L506-L572), vérifier et corriger :

- Longueur supérieure à rx_buffer : rejeter avant parsing ; ne pas transmettre une longueur supérieure aux octets copiés.
- Résultat d'émission dans le bon champ send_err, distinct de recv_err.
- Erreurs RX actuellement seulement journalisées : terminer la transaction correspondante.
- Erreurs immédiates d'envoi : restituer le sémaphore et arrêter le timer correctement.
- Races timer/réponse/fin TX et propriété des buffers lors des annulations.

Les tests CAN amont [utilisent un client ISO-TP direct](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/tests/can/src/main.c#L240-L253) ; ils ne suffisent pas à valider l'API cliente ThingSet corrigée.

## 5. Correctifs du transport report multitrames

### 5.1 Réassemblage et récupération après perte

Le réassemblage existant est défini dans [can.c](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/src/can.c#L55-L109) et son [callback RX](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/src/can.c#L204-L261).

Un nouveau FIRST/SINGLE met à jour le numéro de message, mais ne remet pas à zéro la longueur et la séquence d'un buffer déjà occupé. Après un LAST perdu, le prochain message peut donc être rejeté ou mal concaténé.

Travaux nécessaires :

- Un FIRST valide remplace proprement l'ancien message incomplet de cette source.
- Réinitialiser longueur, séquence et état lors de ce remplacement.
- Refuser CONSEC/LAST sans FIRST.
- Libérer le contexte sur dépassement, erreur de séquence ou expiration.
- Ajouter un timeout ; un FIRST abandonné ne doit pas retenir indéfiniment un buffer.
- Supprimer tout accès au contexte après libération, notamment l'incrément actuel après LAST.
- Remonter des compteurs et, si utile, un événement diagnostique par cause d'abandon.
- Vérifier qu'une erreur n'empêche pas de recevoir le prochain message valide.

Le pool est global et indexé seulement par adresse source. La V1 reste donc sur un bus, une instance et route zéro. Une extension multibus devra ajouter l'instance et la route à l'identité du réassemblage.

### 5.2 Émission et concurrence

Factoriser [l'émetteur existant](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/src/can.c#L264-L345) en un chemin commun de fragmentation pour les reports ThingSet et les payloads privés.

Un verrou par instance doit protéger la totalité d'un message MF : compteur message, séquences, trames et fin d'émission. Deux messages de la même source ne doivent pas s'entrelacer, même s'ils proviennent de threads différents.

Corriger également :

- Le callback TX doit conserver l'erreur du pilote, actuellement ignorée.
- Toute erreur immédiate can_send() doit arrêter l'émission, pas seulement -EAGAIN.
- Un échec de sérialisation ne doit pas retourner un faux succès.
- Un callback tardif après timeout ne doit pas valider la trame d'un message suivant.
- Le délai d'envoi doit être borné, avec compteur et cause d'abandon.
- Le succès du sender signifie émission locale terminée ; il ne prouve pas la réception par toutes les cartes.

### 5.3 Extension proposée, sans canal inventé

Les bits 16 à 23 de l'identifiant servent au routage bus/bridge. Il n'existe pas de champ report_channel contractuellement libre. Les [définitions de routage](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/include/thingset/can.h#L131-L154) doivent rester inchangées.

API nouvelle proposée, non disponible dans le SDK épinglé :

~~~c
int thingset_can_send_raw_report_inst(
    struct thingset_can *instance,
    const uint8_t *data, size_t length,
    k_timeout_t timeout);

int thingset_can_send_raw_report(
    const uint8_t *data, size_t length,
    k_timeout_t timeout);
~~~

Contrat : appel depuis un thread, buffer valide jusqu'au retour, retour après émission ou erreur, taille maximale contrôlée et durée bornée. La méthode existante thingset_can_send_report() doit passer par le même mécanisme de fragmentation et de sérialisation des émissions.

### 5.4 Un seul dispatcher RX, avec copie immédiate

Le callback public thingset_can_set_report_rx_callback() reçoit déjà les octets assemblés. Installer une seule fois un dispatcher qui reconnaît le préfixe privé et conserve le traitement des reports ordinaires.

Le SDK invoque ce callback directement depuis le filtre CAN et libère son buffer immédiatement après son retour. La réception doit donc :

1. Vérifier rapidement la taille minimale et le préfixe.
2. Copier le message dans un élément préalloué d'une file bornée.
3. Enfiler sans attente, puis retourner.
4. Laisser un thread OTA valider le contenu et écrire la flash.
5. En cas de file pleine, compter la perte sans faire avancer rOffset.

Ne jamais conserver uniquement le pointeur reçu. Ne jamais écrire la flash, calculer le hash complet ou attendre une réponse dans le callback CAN.

L'adaptateur de contrôle local [thingset_can.c](../zephyr/modules/owntech_communication/zephyr/src/thingset_can.c) utilise un buffer global et un seul work item. Ce modèle ne convient pas à un flux de blocs OTA ; des messages successifs pourraient écraser le travail en attente.

## 6. Format privé proposé sur le fil

Cette section fige une proposition V1 à implémenter et tester. Elle ne décrit pas un standard ThingSet existant.

En-tête de 32 octets, entiers little-endian, sans copie directe de structure C :

| Offset | Taille | Champ | Valeur ou règle |
|---:|---:|---|---|
| 0 | 4 | magic | Octets ASCII OTAC |
| 4 | 1 | version | 1 |
| 5 | 1 | type | 1 = DATA, 2 = REBOOT |
| 6 | 2 | header_len | 32 |
| 8 | 8 | campaign_id | Identifiant de campagne non réutilisé |
| 16 | 4 | pass_id | Passe active pour DATA |
| 20 | 4 | offset | Offset dans l'artefact pour DATA |
| 24 | 2 | payload_len | Taille utile après l'en-tête |
| 26 | 2 | flags | 0 en V1 |
| 28 | 4 | crc32 | CRC décrit ci-dessous |
| 32 | variable | payload | Octets utiles |

CRC-32/ISO-HDLC : polynôme réfléchi 0xEDB88320, initialisation 0xFFFFFFFF, réflexion entrée/sortie selon ISO-HDLC, xor final 0xFFFFFFFF. Le contrôle connu pour les octets ASCII 123456789 est 0xCBF43926.

Calculer le CRC sur la concaténation header[0:28] || payload. Le champ CRC et les octets éventuels de padding CAN FD sont exclus. La longueur, le type, la campagne, la passe et l'offset sont ainsi protégés contre une corruption accidentelle.

Pour DATA :

- payload_len entre 1 et 256 au démarrage ; le dernier bloc peut être plus court.
- offset + payload_len reste dans la taille exacte annoncée, sans débordement arithmétique.
- campaign_id, source CAN et pass_id correspondent à la campagne préparée.
- Un bloc incompatible ou invalide est rejeté sans progression du writer.

Pour REBOOT :

- pass_id = 0, offset = 0, payload_len = 8, flags = 0.
- Payload : commit_id sur 4 octets puis delay_ms sur 4 octets, little-endian.
- Accepter uniquement à l'état ARMED, pour la campagne et le commit_id enregistrés lors de xArm, depuis la Lead attendue.
- Un doublon du même commit ne repousse jamais une échéance déjà acceptée.
- Le délai relatif vise un redémarrage rapproché ; il ne constitue pas une horloge partagée.

La longueur logique vaut 32 + payload_len. En CAN classique, elle doit correspondre au réassemblage. En CAN FD, accepter uniquement le complément de zéros correspondant à l'arrondi DLC de la dernière trame. Déterminer cet arrondi à partir de la longueur logique et refuser tout suffixe supplémentaire.

Le CRC n'authentifie pas la Lead. Le modèle V1 suppose un bus maîtrisé ; l'authentification et l'anti-rejeu adversarial demandent une conception distincte.

## 7. Inventaire réel des cartes

Les revendications d'adresse transportent l'EUI, mais ne fournissent pas à elles seules un inventaire complet. Une Lead démarrée après les autres peut avoir manqué leurs annonces.

La [procédure actuelle](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/src/can.c#L647-L766) sonde une adresse ciblée et revendique sa propre adresse. Ajouter une découverte active bornée, par sondes espacées des adresses 0x01..0xFD, puis lectures ThingSet.

La [déclaration du callback claim _inst](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/include/thingset/can.h#L283-L324) est limitée à la branche multi-instance du header. Exposer une API propre utilisable en single-instance, avec enregistrement avant la collecte des annonces.

Travaux sur les claims :

- Valider type, adresse et DLC égal à 8 avant de lire l'EUI.
- Copier l'EUI ; le pointeur du callback n'est pas un stockage persistant.
- Associer EUI, adresse, capacités et dernière observation.
- Détecter deux EUI pour la même adresse, ou un changement d'adresse en campagne.
- Geler la liste d'EUI cibles avant PREPARE ; une nouvelle carte ne rejoint pas spontanément.
- Revalider identité et présence avant ARM et après reboot.

Lire au minimum type matériel, version hardware, identité du firmware, version OTA, taille réellement utilisable du slot et capacités CAN. Les capacités OTA sont à ajouter ; les claims seuls ne les contiennent pas.

Le [modèle Device local](../zephyr/modules/owntech_communication/zephyr/src/data_objects.h) expose déjà type/hardware/firmware, mais firmware_version vaut actuellement la constante 1.0.0. La remplacer par une identité issue du build et prévoir la lecture du digest attendu après démarrage.

Le setter [CanCommunication::setCanNodeAddr()](../zephyr/modules/owntech_communication/zephyr/src/CanCommunication.cpp) modifie directement l'adresse sans réinstaller les filtres. Il ne doit pas servir à réadresser une carte pendant une campagne.

## 8. Groupe ThingSet de campagne

Ajouter un groupe applicatif DFUCampaign, avec IDs réservés dans les plages application décrites par [sdk.h](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/include/thingset/sdk.h#L19-L33). Ne pas réutiliser DFU/xBoot, les IDs SDK 0x2D* ou les objets de contrôle 0x8000+.

| Objet proposé | Sémantique |
|---|---|
| xPrepare | Valider métadonnées/identité, entrer en sûreté, initialiser le slot |
| xBeginPass | Ouvrir une passe identifiée et réinitialiser son indicateur de trou |
| xEndPass | Fermer la passe après traitement de la file, rendre le statut stable |
| xFinalize | Flusher puis vérifier taille, hash et en-tête |
| xArm | Marquer une image validée pour l'essai MCUboot |
| xAbort | Abandonner une campagne encore annulable sans promesse de désarmement |
| rCampaignId, rPassId, rState | Identité et progression logique |
| rOffset, rImageSize, rError | Octets acceptés, taille annoncée et erreur |
| rImageHash, rCapabilities | Identité de l'artefact et compatibilité |
| rQueueDepth, compteurs RX | Diagnostic du dimensionnement et des pertes |

Les commandes sont adressées, idempotentes et portent campagne/passe/commit selon leur fonction. Les retries de xPrepare ne doivent jamais effacer une réception déjà engagée de la même campagne.

Un statut ThingSet de succès indique que la commande a été comprise ; les opérations longues sont exécutées par le worker OTA. Le coordinateur attend ensuite READY, PASS_OPEN, PASS_CLOSED, VALID ou ARMED. Documenter les codes de retour immédiats et les erreurs asynchrones séparément.

Les paramètres des fonctions sont copiés dans une commande de travail avant le retour. Éviter de garder le verrou global ThingSet pendant un effacement, un hash ou une attente réseau.

## 9. Déroulement d'une campagne et gestion des passes

1. La Lead vérifie le dépôt USB terminé, l'artefact signé compact, sa taille exacte, son SHA-256 et sa compatibilité.
2. Le coordinateur réserve le slot Lead en lecture exclusive et gèle la liste des EUI.
3. Il adresse xPrepare aux followers, puis attend leurs états READY. La Lead adopte son image déjà déposée ; elle ne passe pas par un effacement de préparation.
4. Chaque cible vérifie l'état MCUboot, réserve son writer et journalise durablement la maintenance avant effacement.
5. La Lead adresse xBeginPass(campaign, pass_id, start_offset), puis attend PASS_OPEN partout.
6. Elle diffuse les DATA de la passe, avec pacing conservateur et aucune réponse par bloc.
7. Elle attend la fin TX locale du dernier report avant d'adresser xEndPass.
8. Chaque cible traite la clôture dans le même ordre que sa file de données et publie PASS_CLOSED après drainage.
9. La Lead lit les offsets stables de toutes les cibles et décide réparation ou finalisation.
10. Elle finalise, vérifie aussi sa propre image, attend tous les états VALID, arme les participants, attend tous les ARMED, puis demande le reboot.

Règles du worker DATA :

- offset == rOffset : vérifier et écrire ; avancer seulement si le writer accepte tout le bloc.
- Bloc entièrement sous rOffset : doublon ignoré.
- Bloc chevauchant l'offset courant : rejeter ; ne pas écrire partiellement.
- offset > rOffset : trou, arrêter les nouvelles écritures de cette passe.
- Erreur CRC, perte dans la file ou erreur de transport : conserver l'offset ; ne jamais inventer les octets absents.
- Un trou reste associé à la passe ; xBeginPass ouvre la possibilité de réparation.
- Une ancienne passe ou un DATA reçu après clôture est ignoré.

rOffset compte les octets acceptés par le writer, y compris ceux encore tamponnés en RAM. Il ne constitue pas un journal de progression durable. Après reset, abandonner le contexte, réconcilier journal et état MCUboot, puis recommencer une préparation complète uniquement si les préconditions de stockage l'autorisent.

Après clôture, calculer repair_offset = min(rOffset des cartes incomplètes). Ouvrir une nouvelle passe et rediffuser ce suffixe. Les cartes déjà plus avancées ignorent les doublons ; les autres reprennent exactement à leur offset. Borner le nombre de passes et la durée totale.

xEndPass ne doit pas répondre « complet » avant la fin des écritures en file. Utiliser une clôture ordonnée dans le worker, ou un mécanisme équivalent explicitement vérifié. Un timeout de clôture bloque la finalisation.

## 10. Image store, DFU legacy et barrières

Le DFU SDK fournit les primitives utiles boot_erase_img_bank, flash_img_init_id et flash_img_buffered_write. Son xBoot actuel ignore le résultat du flush, arme immédiatement puis programme un reboot après une seconde ; il ne constitue pas une finalisation de campagne sûre.

Le service collectif peut utiliser ces primitives sans activer CONFIG_THINGSET_DFU. Le legacy est optionnel. S'il est conservé, ses opérations doivent acquérir le même propriétaire exclusif du slot que le serveur mcumgr et le participant OTA.

Précondition obligatoire de stage_begin côté USB et de xPrepare côté réception : l'image active est confirmée et l'état MCUboot autorise un nouveau transfert. Refuser les états test/non confirmé, pending, revert ou inconnus. Le slot secondaire peut contenir la copie nécessaire au rollback ; sa présence ne signifie pas qu'il est libre.

Avant le premier effacement, enregistrer durablement un journal de maintenance distinct du slot à modifier, puis vérifier la réussite de cet enregistrement. Si le journal est illisible ou incohérent, conserver la maintenance par défaut. La maintenance interdit tout redémarrage automatique de la puissance.

Lire et appliquer ce journal au boot avant tout setup susceptible d'activer la puissance, y compris dans l'ancien firmware et après rollback. Cette logique doit déjà exister dans le firmware initial de toutes les cartes. Le journal contient l'identité de campagne et l'état de maintenance/commit utiles à la réconciliation ; ce n'est pas un journal de reprise du flux firmware.

Après reset, confronter le journal, l'image active et les indicateurs MCUboot. Ne pas reprendre le transfert sur la base de rOffset perdu ; ne pas effacer le slot tant que son rôle n'est pas établi. Une incertitude mène à RECOVERY_REQUIRED. La sortie de maintenance suit une procédure explicite après les contrôles de santé.

Propriétaires distincts à gérer : upload USB, réception legacy, réception campagne, lecture/distribution Lead. Aucun deuxième chemin ne doit effacer ou remplacer l'image en cours de distribution ou déjà armée.

États minimaux :

IDLE → PREPARING → READY → PASS_OPEN → PASS_CLOSED → VERIFYING → VALID → ARMED → REBOOTING

PASS_CLOSED peut retourner vers PASS_OPEN pour une réparation. Une erreur avant armement mène à FAILED ou ABORTED en conservant l'image active. Une situation incertaine après armement mène à RECOVERY_REQUIRED.

Finalisation :

- Fermer les passes et vérifier que rOffset égale la taille attendue.
- Flusher le writer une seule fois dans xFinalize, après drainage et rOffset égal à la taille. Ne pas flusher au dernier DATA puis reflusher à la finalisation.
- Vérifier le résultat de ce flush ; un retry xFinalize pendant/après son exécution renvoie l'état existant sans le répéter.
- La Lead vérifie que son writer USB est fermé, puis valide l'image déjà déposée ; elle ne crée pas un writer de réception pour la reflusher.
- Relire la flash et comparer le SHA-256 sur les octets exacts de l'artefact.
- Vérifier les limites du slot, l'en-tête MCUboot et la compatibilité attendue.
- Ne pas confondre ce hash de transport avec une vérification complète de signature.
- Laisser MCUboot effectuer la validation autoritative au démarrage.

Armement :

- N'appeler boot_request_upgrade(BOOT_UPGRADE_TEST) que sur une image VALID.
- Retourner ARMED seulement après succès effectif.
- Ne pas réeffacer un slot armé lors d'un retry ou d'un abort tardif.
- Si un sous-ensemble est armé et qu'une autre carte échoue, déclarer RECOVERY_REQUIRED.
- Une carte armée peut activer l'image si elle redémarre accidentellement avant le reboot collectif.
- Après reboot, confirmer l'image locale seulement après santé locale réussie, puis vérifier identité, version réelle, santé et éventuel retour arrière de toutes les cartes. La confirmation locale ne prouve pas le succès de la flotte.

La barrière garantit une politique de coordination avant activation volontaire ; elle ne garantit pas le « tout ou rien » sous toutes les pannes. Le point de confirmation de l'application existante doit être revu dans le chantier système.

## 11. Configuration, ressources et CAN FD

Le module OwnTech [Kconfig](../zephyr/modules/owntech_communication/zephyr/Kconfig) sélectionne CAN/ThingSet et la réception d'items simples, mais pas la réception MF ni le service collectif.

Point de départ à mesurer :

- CONFIG_THINGSET_CAN_REPORT_RX=y.
- CONFIG_THINGSET_CAN_REPORT_RX_BUFFER_SIZE=512 pour 256 octets utiles et l'enveloppe proposée.
- Nombre de buffers tenant compte de toutes les sources de reports autorisées, pas uniquement de la Lead.
- File applicative de blocs préalloués, dimensionnée à partir de la latence flash mesurée.
- Inter-trames et inter-blocs conservateurs, sans réception bloquante.
- CONFIG_THINGSET_CAN_RX_BUF_SIZE dimensionné séparément pour les requêtes et réponses.
- Legacy DFU désactivé par défaut si son ownership n'est pas encore adapté.

Le [Kconfig SDK](https://github.com/ThingSet/thingset-zephyr-sdk/blob/e57447bbeb7c165e14a273242e8495343c1c6f54/src/Kconfig.can#L14-L82) limite les buffers report à 1024 octets ; leur défaut de 64 octets est insuffisant pour le format proposé.

Le sender actuel active CAN_FRAME_FDF, sans activer CAN_FRAME_BRS. Déclarer un bitrate data dans le devicetree ne suffit donc pas à obtenir ce débit sur ces reports. Valider séparément CAN classique, FD sans BRS et FD avec BRS, selon le matériel et les corrections retenues.

Mesurer RAM, flash, files, erreurs, durée d'effacement, durée d'écriture et débit. Un acquittement CAN physique ne prouve ni la réception par toutes les applications ni la durabilité en flash.

## 12. Lots d'implémentation et critères de sortie

| Lot | Modifications futures | Critère de sortie |
|---|---|---|
| T0 | Fork et versions reproductibles | Sources/révisions et configuration exactes documentées |
| T1 | Client CAN, tailles et ownership TX | Vraie réponse attendue ; une seule fin de transaction |
| T2 | Réassemblage MF, timeout, diagnostics | Perte d'une trame suivie d'un message valide récupérée |
| T3 | Sender raw commun, codec OTAC, dispatcher | Deux récepteurs reconstruisent des données de test |
| T4 | Inventaire actif et modèle ThingSet | Cartes déjà démarrées identifiées par EUI |
| T5 | Worker, image store, commandes de passe | Aucun appel flash en ISR ; statut après drainage |
| T6 | Streaming et suffix repair | Pertes injectées réparées avec offsets exacts |
| T7 | Finalisation, ARM et REBOOT | Barrières et état de récupération vérifiés |
| T8 | Confirmation et essais système | Versions/santé et comportement sous reset observés |

Ces lots s'intègrent aux étapes USB, génération d'artefact et MCUboot du rapport principal. T1 et T2 précèdent toute campagne réelle ; un test unicast réussi depuis Python ne remplace pas ces validations.

## 13. Tests ciblés avant toute qualification

| Scénario | Résultat attendu |
|---|---|
| Requête courte puis réponse différée | Fin TX ne termine pas la transaction |
| Réponse longue, erreur RX, timeout | Taille contrôlée et callback terminal unique |
| Réponse tardive pendant retry | Pas de fausse validation de campagne/passe |
| Client et réponse serveur simultanés | Aucun déverrouillage du buffer d'un autre propriétaire |
| LAST perdu puis nouveau FIRST | Nouveau message reçu sans concaténation |
| Wrap de séquence/message | Aucun bloc incomplet accepté comme valide |
| FIRST abandonnés de plusieurs sources | Expiration et restitution du pool |
| File applicative pleine | Perte comptée, offset conservé, réparation possible |
| Payload FD avec padding | CRC calculé sur les octets utiles seulement |
| Report normal entre blocs OTA | Dispatcher et sérialisation préservent les deux usages |
| PREPARE ou Begin/EndPass répété | Aucun effacement ou changement de passe intempestif |
| EndPass reçu pendant une écriture | Statut final publié après drainage |
| Perte du dernier DATA entier | Offset incomplet détecté à la clôture |
| Ancien pass_id et campagne inconnue | Données rejetées sans écriture |
| Upload USB ou legacy durant distribution | Conflit d'ownership explicite |
| Reset avant ARM | Journal/MCUboot réconciliés, maintien en maintenance, aucune reprise RAM présumée |
| Nouveau dépôt avec image active test/pending/revert | Refus avant effacement ; copie de rollback préservée |
| Journal illisible ou reset après journalisation | Maintenance restaurée avant toute activation de puissance |
| xFinalize répété ou dernier DATA reçu | Un seul flush après drainage ; résultat/état réutilisé |
| Échec après ARM partiel | RECOVERY_REQUIRED, aucune promesse d'annulation |
| REBOOT répété | Échéance déjà acceptée non repoussée |
| Firmware de signature invalide | Refus MCUboot observé et état de flotte explicite |
| Post-boot avec ancien firmware | Différence détectée par identité réelle de l'artefact |

Les tests de codec et d'état peuvent être automatisés sans flash. Les contraintes de débit, sûreté matérielle, effacement, interruption d'alimentation, boot et confirmation exigent ensuite un banc avec une Lead et au moins deux cartes cibles.
