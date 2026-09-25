# MMC_ANA : audit d'intégration du récepteur minimal

Audit source du 25 septembre 2026, sans modification de MMC_ANA. Le fichier
`C:/Users/afarahhass/Documents/Repo/MMC/MMC_ANA/src/main.cpp` a le SHA-256
`36B01F28C82D51DEA740A62825ECB60CC4F7AAB0753118B8B2077E18C2E9938D`, identique
à la référence du plan. Cet audit ne qualifie ni le matériel ni le temps réel.

## Voies de commande inspectées

- Les commandes RS485 changent `mode` en POWERMODE/IDLEMODE aux lignes
  654–660 ; la console peut également changer ce mode aux lignes 797–801.
- La tâche critique appelle `shield.power.start(LEG1)` aux lignes 989 et 1014,
  ainsi que `setDutyCycle` et `stop`. Les API Power, les deux fonctions
  `hrtim_out_en`/`hrtim_out_en_single` et les écritures GPIO de drivers/condensateurs
  comportent déjà des gardes `ota_safety_inhibited()`. Les écritures HRTIM/GPIO
  sont protégées avec `irq_lock`, ce qui ferme la fenêtre entre vérification
  et activation. L'arrêt OTA vérifie OENR et relit les GPIO concernés.
- Aucun accès direct à OENR, aucune commande LL/HAL d'activation HRTIM et aucun
  GPIO direct de puissance n'ont été trouvés dans `MMC_ANA/src`. Les accès LL
  directs aux lignes 560–574 concernent uniquement la LED PA5. Cette LED directe
  contourne toutefois l'arbitrage LED de Core ; l'indication OTA ne constitue
  donc pas une preuve de l'état de maintenance de MMC.
- La tâche critique assure aussi mesures, calculs et communications. L'arrêter
  globalement ne constitue pas un adaptateur de sûreté acceptable. Scope garde
  sa profondeur, ses 14 canaux et son allocation. Aucun protocole RS485 n'est modifié.

## Points qui bloquent un adaptateur automatique sûr

1. `pwm_enable` est un état privé `static` de MMC (ligne 490). MMC le passe à
   `true` *avant* d'appeler `shield.power.start` (lignes 988 et 1013). Pendant
   l'inhibition, Core refuse l'activation mais MMC croit alors avoir démarré.
   À la libération, son test `if (!pwm_enable)` peut empêcher toute reprise.
   Core ne dispose d'aucun acquittement lui permettant de réconcilier cet état
   sans un contrat applicatif explicite. Relancer automatiquement les sorties
   dans Core à la libération pourrait réappliquer une commande RS485 ancienne.
2. Le démarrage de la tâche critique (ligne 739) précède la configuration RS485
   (743–745), la synchronisation et Scope (749–772). Observer « tâche créée »
   ou « HRTIM initialisé » ne prouve donc pas que l'initialisation MMC est
   achevée. Il n'existe pas de signal public de santé/initialisation complète
   permettant à Core de confirmer une image d'essai.
3. MMC peut continuer à transmettre POWER sur RS485 pendant l'inhibition
   locale. Les gardes protègent les sorties locales, mais l'arrêt collectif
   des équipements hors campagne et la persistance de leur supervision
   nécessitent une qualification du système ; un succès du lien ne le prouve pas.

En conséquence, les callbacks faibles `owntech_ota_enter_maintenance()` et
`owntech_ota_check_health()` restent en refus par défaut. Aucun succès
inconditionnel n'est ajouté pour MMC. Un build de dimensionnement de MMC avec
le récepteur minimal est possible, mais ne constitue pas un firmware MMC OTA
opérationnel : confirmation et admission restent bloquées tant que ces preuves
et le contrat de reprise ne sont pas disponibles. La démo LED et le Lead dédié
ont leurs propres callbacks limités à leurs comportements sans puissance.

## USB et activité hors campagne

Le récepteur dispose d'un statut en lecture seule sur la console CDC existante :
un changement à **2400 bauds** programme une unique réponse `OTAR2 {JSON}\n`.
Aucun octet de requête n'est injecté dans l'entrée console, aucun lecteur ne
vole les commandes de MMC et aucun SMP applicatif n'est ajouté. La réponse
occupe au plus 768 octets ; les demandes rapprochées sont coalescées. Un seul
appel FIFO non bloquant préserve le comportement même si l'hôte ne lit pas.
Une console saturée peut tronquer ou entrelacer une réponse : le PC ignore la
ligne invalide et réessaie explicitement, sans reset ni upload implicite.

Le worker du récepteur attend `K_FOREVER` en dehors des événements. Il ne
scrute aucune flotte, conserve une seule identité Lead et utilise une union
commande/données dans sa file bornée. La LED utilise une workqueue et des
échéances seulement pendant les séquences clignotantes ; les états stables
n'entretiennent aucun réveil périodique. Les appels LED depuis ISR restent
non bloquants. La gigue HRTIM, les pertes RS485 et les points hauts des piles
restent à mesurer sur matériel, y compris sous trafic CAN perturbateur.
