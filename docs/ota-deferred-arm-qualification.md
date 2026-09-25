# Qualification de l'activation différée

Audit de sources et de preuves locales du 25 septembre 2026. Aucun accès matériel,
effacement ou nouvel essai de bootloader n'a été effectué pendant cet audit.
`CONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED` reste désactivé dans les profils livrés.
La compilation avec cette option activée dans les tests et la copie de mesure
inclut le chemin complet de stockage ; elle ne qualifie pas une carte.

## Ce qui a été vérifié

Le [partitionnement Core](../zephyr/boards/owntech/spin/spin.dts) et le devicetree
généré utilisent des écritures de 8 octets, des pages de 2 048 octets, et deux
slots de 227 328 octets (`0x37800`). Le secondaire est
`[0x08047800, 0x0807f000)` ; NVS commence à `0x0807f000`.
La borne signée compacte de 221 184 octets (`0x36000`) réserve les trois dernières
pages du secondaire, soit `[0x0807d800, 0x0807f000)`.

Le [writer](../zephyr/modules/owntech_ota/zephyr/src/ota_storage.cpp) efface tout
le secondaire après les contrôles de disponibilité, maintenance et réserve NVS.
Il vérifie l'effacement avant de recevoir et n'accepte que les octets exacts de
l'artefact compact. Il exige aussi que sa longueur arrondie au buffer de 512
octets reste dans la borne utile. Dans la bibliothèque Zephyr locale,
`subsys/storage/stream/stream_flash.c` arrondit la dernière écriture à l'unité
matérielle de 8 octets. La borne du writer est donc conservatrice, sans réduire
la capacité utile puisque `0x36000` est divisible par 512.
La configuration effective désactive `IMG_ERASE_PROGRESSIVELY` et
`STREAM_FLASH_ERASE` : l'effacement initial explicite est le seul effacement du
transfert. Aucun padding de fichier ne programme le trailer.

Après validation du hash de transfert, des TLV et de la classe protégée `0xA0`,
le participant persiste et relit `COMMIT_INTENT`, appelle
`boot_request_upgrade(BOOT_UPGRADE_TEST)`, vérifie `mcuboot_swap_type()==TEST`,
puis persiste et relit `COMMITTED`. Une erreur ne remplace pas l'intention par
une phase précommit. Les reprises de commandes ne réarment pas le slot.

La bibliothèque liée par l'application provient de
`framework-zephyr/_pio/bootloader/mcuboot`, dont le README annonce MCUboot 2.1.0.
Dans `boot/bootutil/src/bootutil_public.c`, `boot_set_pending()` utilise
`boot_set_next()` : il écrit d'abord la magie du secondaire, puis `swap_info`
pour un essai ; il n'écrit pas `image_ok`. Ces écritures ne sont pas atomiques.
Une coupure entre elles peut donc avoir déjà rendu la candidate amorçable.
C'est pourquoi l'intention durable précède leur première écriture et le succès
CAN suit leur résultat durable. Le stockage relit le trailer effacé juste
avant l'appel, car cette version de `boot_set_next()` peut effacer un secondaire
dont la magie est corrompue. Une corruption détectée est refusée sans appeler
cette récupération implicite.

Les gardes de disponibilité utilisent l'état réel renvoyé par
`boot_is_img_confirmed()` et `mcuboot_swap_type()`, en plus du propriétaire OTA.
Le premier traite également une image initiale sans magie comme confirmée,
selon la convention documentée de Zephyr. Ces API interprètent le trailer ;
elles ne mesurent ni la géométrie du bootloader installé, ni les bits ECC, ni
l'état de puissance de l'application.

## Preuves historiques et limite de leur portée

Les caches `.pio/owntech-boot-*-v1.1.0.*` contiennent le `prj.conf`, le CMake et
l'entrée du bootloader OwnTech v1.1.0, dont le README annonce MCUboot 2.1.0-dev.
Le profil met `BOOT_UPGRADE_ONLY=n`, `BOOT_BOOTSTRAP=n` et
`BOOT_VALIDATE_SLOT0=n`. Le CMake prévoit explicitement le swap par déplacement
et réserve son secteur supplémentaire. Ces fichiers ne sont pas le `.config`
effectif du binaire installé et ne prouvent pas, seuls, ses paramètres finaux.

Les deux sauvegardes historiques de 65 536 octets,
`.pio/swd-recovery-20260924/bootloader.bin` et
`.pio/swd-board2-20260924/bootloader.bin`, ont été relues pendant cet audit :

```text
SHA-256 b754ee9bff9b84121261f52bef9dadfa60e2681e39494f469074478986e436ff
```

Le [rapport de récupération existant](ota-recovery.md#second-board-interrupted-revert-and-usb-repair)
identifie ce binaire comme OwnTech v1.1.0 et décrit les progressions de swap par
déplacement observées. L'analyse historique indique un début de statut relatif
au slot à `0x36bd0` et un alignement de trailer de 8 octets, compatibles avec
la réserve de trois pages. Les écritures de confirmation après programmation
clairsemée ont également été observées. Cela constitue une preuve de l'ancien
banc, pas un cycle compact avec activation différée, ni une identification
actuelle des bootloaders d'autres cartes. Les caches et dumps sont ignorés par
Git : les archiver avec le dossier de qualification, sans dépendre de `.pio`.

## Conditions pour lever la garde

Sur le bootloader effectivement installé, il reste à démontrer :

1. La correspondance du binaire, de la clé, du partitionnement, du swap par
   déplacement, de l'alignement et de la réserve de trailer. L'identifiant
   logiciel `bootloader_id` est un contrat de provisionnement, pas une attestation
   du contenu physique du bootloader.
2. L'absence d'activation après effacement, transfert partiel, transfert complet
   et `VALID`, avec coupures d'alimentation à chacune de ces étapes.
3. Les coupures avant/après `COMMIT_INTENT`, pendant la magie et `swap_info`,
   puis avant/après `COMMITTED`. Le hash réellement démarré doit être vérifié et
   les sorties doivent rester inhibées jusqu'à la réconciliation collective.
4. La confirmation et le retour à l'ancienne image, signature invalide incluse,
   puis deux campagnes consécutives. La copie effectuée par le bootloader doit
   elle aussi conserver programmables les cellules ECC nécessaires aux futurs
   marqueurs. Une lecture `FF` ne prouve pas leur effacement physique.
5. Le nouveau TLV protégé de classe sur ce binaire installé, ainsi que les voies
   USB d'installation et de récupération ciblée v2. Le code local MCUboot 2.1.0
   accepte les TLV inconnus protégés ; cela ne remplace pas l'essai de la version
   OwnTech installée.

Les tests hôtes exercent les états, limites, CRC, doubles hashes, réserve NVS,
échecs de persistance, coupures simulées et refus de récupération après intention
de commit. Leurs Flash/NVS/MCUboot sont simulés : ils ne constituent pas ces cinq
preuves matérielles. Les garanties MMC et temporelles restent indépendantes,
décrites dans [l'audit de sûreté MMC](ota-mmc-safety-audit.md).
