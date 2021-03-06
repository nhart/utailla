<?php
/**
 * @file
 * Resource migration.
 */

/**
 * Batch opp for migrating resources.
 */
function utailla_migrate_resources_batch_operation(&$context) {
  module_load_include('inc', 'utailla', 'includes/migration.db');
  $sandbox =& $context['sandbox'];
  if (!isset($sandbox['total'])) {
    $sandbox['progress'] = 0;
    $sandbox['total'] = utailla_get_resource_count();
  }

  $resources = utailla_get_resource_info($sandbox['progress']);
  foreach ($resources as $resource) {
    utailla_migrate_resource($resource);
    $sandbox['progress']++;
  }

  $context['finished'] = $sandbox['progress'] / $sandbox['total'];
  $context['message'] = t('Processed @progress of @total.', array(
    '@progress' => $sandbox['progress'],
    '@total' => $sandbox['total'],
  ));
}

/**
 * Migrate an resource if it hasn't already been.
 */
function utailla_migrate_resource($resource_info) {
  module_load_include('inc', 'utailla', 'includes/migration.db');
  module_load_include('inc', 'utailla', 'includes/utilities');
  if (!utailla_migrated_resource_pid($resource_info->resource_id)) {
    $resource = utailla_get_decoded_row_recursive($resource_info, array('citations', 'description_text'));

    $object = utailla_get_new_object();
    $object->models = array(UTAILLA_RESOURCE_CMODEL);
    // Add all collection relationships.
    if ($resource['collections']) {
      foreach ($resource['collections'] as $collection) {
        $pid = utailla_migrated_collection_pid($collection['collection_id']);
        if ($pid) {
          $object->relationships->add(FEDORA_RELS_EXT_URI, 'isMemberOfCollection', $pid);
        }
      }
    }
    // Note: some of the data being migrated has empty title_text.
    if ($resource['titles']) {
      // Try to set the label to the english title.
      foreach ($resource['titles'] as $title) {
        if ($title['language'] == 'en' && !empty($title['title_text'])) {
          $object->label = $title['title_text'];
          break;
        }
      }
      // Try to set the label to the spanish title.
      if (!$object->label) {
        foreach ($resource['titles'] as $title) {
          if ($title['language'] == 'sp' && !empty($title['title_text'])) {
            $object->label = $title['title_text'];
            break;
          }
        }
        // Try to set the label to the other title.
        if (!$object->label) {
          foreach ($resource['titles'] as $title) {
            if ($title['language'] == 'other' && !empty($title['title_text'])) {
              $object->label = $title['title_text'];
              break;
            }
          }
          // If it's still not set use "Resource" as label.
          if (!$object->label) {
            $object->label = 'Resource';
          }
        }
      }
    }

    $mods_ds = $object->constructDatastream('MODS', 'M');
    $mods_ds->label = 'MODS Record';
    $mods_ds->mimetype = 'application/xml';
    $mods_ds->content = utailla_build_resource_mods($resource);
    $object->ingestDatastream($mods_ds);

    // Add media file collection policy.
    utailla_set_collection_policy($object, 'media_collection_policy.xml');
    islandora_add_object($object);

    utailla_log_resource_mapping($resource['resource_id'], $object->id);
    drupal_set_message(
      t(
        'Constructed an object for the resource @resource with PID @pid.',
        array('@resource' => $resource['resource_id'], '@pid' => $object->id)
      )
    );
  }
  else {
    drupal_set_message(
      t(
        'An object already exists for the resource @resource.',
        array('@resource' => $resource_info->resource_id)
      )
    );
  }
}

/**
 * Get content for an resource's MODS DS.
 */
function utailla_build_resource_mods($resource) {
  module_load_include('inc', 'utailla', 'includes/utilities');
  module_load_include('inc', 'utailla', 'includes/migration.db');

  $writer = utailla_get_mods_writer();

  // Titles.
  if ($resource['titles']) {
    foreach ($resource['titles'] as $title) {
      if (!empty($title['title_text'])) {
        $writer->startElement('titleInfo');

        switch ($title['language']) {
          case 'other':
            $writer->writeAttribute('displayLabel', 'Title (Indigenous language)');
            $writer->writeAttribute('type', 'alternative');
            break;

          case 'en':
            $writer->writeAttribute('displayLabel', 'Title (English)');
            $writer->writeAttribute('lang', 'eng');
            break;

          case 'sp':
            $writer->writeAttribute('displayLabel', 'Title (Spanish)');
            $writer->writeAttribute('lang', 'spa');
            break;

        }
        $writer->startElement('title');
        $writer->text($title['title_text']);
        $writer->endElement();
        $writer->endElement();
      }
    }
  }

  // Contributors and role.
  foreach ($resource['contributors'] as $contributor) {
    $pid = utailla_migrated_contributor_pid($contributor['contributor_id']);
    $object = islandora_object_load($pid);
    if (isset($object['MADS'])) {
      // Get the namepart data out of MADS.
      $name_data = utailla_get_contributor_nameparts_from_mads($object['MADS']->content);

      $writer->startElement('name');
      $writer->writeAttribute('type', 'personal');
      $writer->writeAttribute('authority', 'aillaPerson');
      $writer->writeAttribute('displayLabel', 'Contributor (Individual)');
      $writer->writeAttribute('valueURI', utailla_get_uri_from_pid($pid));

      // Contributor Given Name.
      $writer->startElement('namePart');
      $writer->writeAttribute('type', 'given');
      $writer->text($name_data['given_name']);
      $writer->endElement();

      // Contributor Family Name.
      $writer->startElement('namePart');
      $writer->writeAttribute('type', 'family');
      $writer->text($name_data['family_name']);
      $writer->endElement();

      // Role in English.
      $writer->startElement('role');
      $writer->startElement('roleTerm');
      $writer->writeAttribute('type', 'text');
      $writer->writeAttribute('lang', 'eng');
      $writer->writeAttribute('authority', 'aillaRoleTerms');
      $writer->text($contributor['role_name']);
      $writer->endElement();
      $writer->endElement();

      // Role in spanish.
      $writer->startElement('role');
      $writer->startElement('roleTerm');
      $writer->writeAttribute('type', 'text');
      $writer->writeAttribute('lang', 'spa');
      $writer->writeAttribute('authority', 'aillaRoleTerms');
      $writer->text($contributor['papel_nombre']);
      $writer->endElement();
      $writer->endElement();

      $writer->endElement();
    }
  }

  // Organization and role.
  foreach ($resource['organizations'] as $organization) {
    $pid = utailla_migrated_organization_pid($organization['organization_id']);
    $object = islandora_object_load($pid);
    if ($object) {
      $writer->startElement('name');
      $writer->writeAttribute('type', 'corporate');
      $writer->writeAttribute('authority', 'aillaOrganization');
      $writer->writeAttribute('displayLabel', 'Contributor (Organization)');
      $writer->writeAttribute('valueURI', utailla_get_uri_from_pid($pid));

      // Contributor Given Name.
      $writer->startElement('namePart');
      $writer->text($object->label);
      $writer->endElement();

      // Role in English.
      $writer->startElement('role');
      $writer->startElement('roleTerm');
      $writer->writeAttribute('type', 'text');
      $writer->writeAttribute('lang', 'eng');
      $writer->writeAttribute('authority', 'aillaRoleTerms');
      $writer->text($organization['role_name']);
      $writer->endElement();
      $writer->endElement();

      // Role in spanish.
      $writer->startElement('role');
      $writer->startElement('roleTerm');
      $writer->writeAttribute('type', 'text');
      $writer->writeAttribute('lang', 'spa');
      $writer->writeAttribute('authority', 'aillaRoleTerms');
      $writer->text($organization['papel_nombre']);
      $writer->endElement();
      $writer->endElement();

      $writer->endElement();
    }
  }

  // Languages.
  foreach ($resource['languages'] as $language) {
    $pid = utailla_migrated_language_pid($language['language_id']);
    if ($pid) {
      $writer->startElement('language');
      $writer->startElement('languageTerm');
      $writer->writeAttribute('type', 'text');
      $writer->writeAttribute('authority', 'aillaLanguage');
      $writer->writeAttribute('valueURI', utailla_get_uri_from_pid($pid));
      $writer->endElement();
      $writer->endElement();
    }
  }

  // Country.
  if ($resource['country_id']) {
    $pid = utailla_migrated_country_pid($resource['country_id']);
    if ($pid) {
      $writer->startElement('originInfo');
      $writer->writeAttribute('eventType', 'production');
      $writer->startElement('place');
      $writer->startElement('placeTerm');
      $writer->writeAttribute('type', 'text');
      $writer->writeAttribute('authority', 'aillaCountry');
      $writer->writeAttribute('displayLabel', 'Country');
      $writer->writeAttribute('valueURI', utailla_get_uri_from_pid($pid));
      $writer->endElement();
      $writer->endElement();
      $writer->endElement();
    }
  }

  // Descriptions.
  if ($resource['descriptions']) {
    foreach ($resource['descriptions'] as $description) {
      $writer->startElement('abstract');
      switch ($description['language']) {
        case 'en':
          $writer->writeAttribute('lang', 'eng');
          $writer->writeAttribute('displayLabel', 'Resource description (English)');
          break;

        case 'sp':
          $writer->writeAttribute('lang', 'spa');
          $writer->writeAttribute('displayLabel', 'Resource description (Spanish)');
          break;

      }
      $writer->text($description['description_text']);
      $writer->endElement();
    }
  }

  // Legacy Resource ID.
  if ($resource['identifier']) {
    $writer->startElement('identifier');
    $writer->writeAttribute('type', 'local');
    $writer->writeAttribute('displayLabel', 'Legacy Resource ID');
    $writer->text($resource['identifier']);
    $writer->endElement();
  }

  // Collection Name.
  if ($resource['collections']) {
    foreach ($resource['collections'] as $collection) {
      $pid = utailla_migrated_collection_pid($collection['collection_id']);
      $collection = islandora_object_load($pid);
      if ($collection) {
        $writer->startElement('relatedItem');
        $writer->writeAttribute('type', 'host');
        $writer->startElement('titleInfo');
        $writer->writeAttribute('displayLabel', 'Collection name');
        $writer->startElement('title');
        $writer->writeAttribute('authority', 'aillaCollection');
        $writer->writeAttribute('valueURI', utailla_get_uri_from_pid($pid));
        $writer->text($collection->label);
        $writer->endElement();
        $writer->endElement();
        $writer->endElement();
      }
    }
  }

  // Language Community.
  if ($resource['language_community']) {
    $writer->startElement('note');
    $writer->writeAttribute('type', 'language community');
    $writer->text($resource['language_community']);
    $writer->endElement();
  }
  // Source Note.
  if ($resource['original_identifier']) {
    $writer->startElement('note');
    $writer->writeAttribute('type', 'source note');
    $writer->text($resource['original_identifier']);
    $writer->endElement();
  }

  // Place.
  if ($resource['place']) {
    $writer->startElement('originInfo');
    $writer->writeAttribute('eventType', 'production');
    $writer->startElement('place');
    $writer->startElement('placeTerm');
    $writer->writeAttribute('type', 'text');
    $writer->writeAttribute('displayLabel', 'Place created');
    $writer->text($resource['place']);
    $writer->endElement();
    $writer->endElement();
    $writer->endElement();
  }

  // Date Created.
  if ($resource['date_created']) {
    $writer->startElement('originInfo');
    $writer->writeAttribute('eventType', 'production');
    $writer->startElement('dateCreated');
    $writer->text($resource['date_created']);
    $writer->endElement();
    $writer->endElement();
  }

  // Type of Resource.
  $writer->startElement('typeOfResource');
  $writer->text('mixed material');
  $writer->endElement();

  // Genres.
  if ($resource['genres']) {
    foreach ($resource['genres'] as $genre) {
      $writer->startElement('genre');
      $writer->writeAttribute('lang', 'eng');
      $writer->writeAttribute('type', 'local');
      $writer->writeAttribute('authority', 'aillaResourceGenres');
      $writer->text($genre['name']);
      $writer->endElement();

      $writer->startElement('genre');
      $writer->writeAttribute('lang', 'spa');
      $writer->writeAttribute('type', 'local');
      $writer->writeAttribute('authority', 'aillaResourceGenres');
      $writer->text($genre['nombre']);
      $writer->endElement();
    }
  }

  // Reference/Citation.
  if ($resource['citations']) {
    $citations = preg_split('/<br\s*\/?>/i', $resource['citations']);
    foreach ($citations as $citation) {
      $writer->startElement('note');
      $writer->writeAttribute('type', 'citation/reference');
      $writer->text($citation);
      $writer->endElement();
    }
  }

  $writer->endElement();
  $writer->endDocument();
  return $writer->flush();
}
